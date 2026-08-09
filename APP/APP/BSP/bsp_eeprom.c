/* ================================================================
 * AT24C02 驱动实现（软件模拟 IIC：PB8=SCL / PB9=SDA，7 位地址 0x50）
 *   移植自正点原子《探索者 STM32F407 开发指南》IIC 实验（myiic.c）：
 *   板载 24C02 的 SCL/SDA 分别连接 PB8/PB9，官方用 GPIO 位操作
 *   软件模拟 IIC 驱动（非硬件 I2C 外设）。
 *   - 256B，8B 页写，任意字节就地改写（非 flash 仅清位）；
 *   - 与 MPU6050（硬件 I2C1 / PB6-PB7）完全独立，互不干扰；
 *   - 内部互斥串行化；写周期等待 ≤5ms。
 * ================================================================ */
#include "bsp_eeprom.h"
#include "main.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "cmsis_os2.h"

#define EEPROM_SCL_PORT      GPIOB
#define EEPROM_SCL_PIN       GPIO_PIN_8
#define EEPROM_SDA_PORT      GPIOB
#define EEPROM_SDA_PIN       GPIO_PIN_9
#define EEPROM_SDA_MODE_POS  18u        /* PB9 在 MODER 的位偏移 9*2 */

#define EEPROM_WRITE_ADDR    (BSP_EEPROM_ADDR & 0xFEu)   /* 0xA0 */
#define EEPROM_READ_ADDR     (BSP_EEPROM_ADDR | 0x01u)   /* 0xA1 */

#define EEPROM_IO_US         2u         /* 字节内位时序半周期（约 250kHz） */
#define EEPROM_EDGE_US       4u         /* 起始/停止建立保持（官方 delay_us(4)） */
#define EEPROM_ACK_TIMEOUT   500u       /* ACK 等待超时（×1us） */
#define EEPROM_WRITE_CYCLE   5          /* AT24C02 写周期 ≤5ms */

static SemaphoreHandle_t s_lock = NULL;

/* ---------- DWT 微秒延时（168MHz） ---------- */
static void eeprom_delay_us(uint32_t us)
{
    uint32_t cyc = us * 168u;
    uint32_t t0 = DWT->CYCCNT;
    while ((DWT->CYCCNT - t0) < cyc) {
    }
}

/* ---------- 引脚原语 ---------- */
static void scl_hi(void)
{
    HAL_GPIO_WritePin(EEPROM_SCL_PORT, EEPROM_SCL_PIN, GPIO_PIN_SET);
}

static void scl_lo(void)
{
    HAL_GPIO_WritePin(EEPROM_SCL_PORT, EEPROM_SCL_PIN, GPIO_PIN_RESET);
}

static void sda_hi(void)
{
    HAL_GPIO_WritePin(EEPROM_SDA_PORT, EEPROM_SDA_PIN, GPIO_PIN_SET);
}

static void sda_lo(void)
{
    HAL_GPIO_WritePin(EEPROM_SDA_PORT, EEPROM_SDA_PIN, GPIO_PIN_RESET);
}

static void sda_out(void)
{
    GPIOB->MODER = (GPIOB->MODER & ~(3u << EEPROM_SDA_MODE_POS)) |
                   (1u << EEPROM_SDA_MODE_POS);
}

static void sda_in(void)
{
    GPIOB->MODER = (GPIOB->MODER & ~(3u << EEPROM_SDA_MODE_POS));
}

static uint8_t sda_read(void)
{
    return (HAL_GPIO_ReadPin(EEPROM_SDA_PORT, EEPROM_SDA_PIN) == GPIO_PIN_SET)
               ? 1u : 0u;
}

/* ---------- 软 IIC 时序（官方 myiic.c 移植） ---------- */
static void iic_start(void)
{
    sda_out();
    sda_hi();
    scl_hi();
    eeprom_delay_us(EEPROM_EDGE_US);
    sda_lo();
    eeprom_delay_us(EEPROM_EDGE_US);
    scl_lo();
}

static void iic_stop(void)
{
    sda_out();
    sda_lo();
    scl_hi();
    eeprom_delay_us(EEPROM_EDGE_US);
    sda_hi();
    eeprom_delay_us(EEPROM_EDGE_US);
    scl_lo();
}

static int iic_wait_ack(void)
{
    uint32_t t = 0;
    sda_in();
    scl_hi();
    eeprom_delay_us(EEPROM_IO_US);
    while (sda_read() && t < EEPROM_ACK_TIMEOUT) {
        eeprom_delay_us(1u);
        t++;
    }
    scl_lo();
    eeprom_delay_us(EEPROM_IO_US);
    sda_out();
    return (t < EEPROM_ACK_TIMEOUT) ? 0 : -1;
}

static int iic_send_byte(uint8_t data)
{
    sda_out();
    scl_lo();
    for (uint8_t i = 0; i < 8; i++) {
        if (data & 0x80u) {
            sda_hi();
        } else {
            sda_lo();
        }
        data <<= 1;
        eeprom_delay_us(EEPROM_IO_US);
        scl_hi();
        eeprom_delay_us(EEPROM_IO_US);
        scl_lo();
        eeprom_delay_us(EEPROM_IO_US);
    }
    return iic_wait_ack();
}

static uint8_t iic_read_byte(uint8_t ack)
{
    uint8_t data = 0;
    sda_in();
    for (uint8_t i = 0; i < 8; i++) {
        data <<= 1;
        eeprom_delay_us(EEPROM_IO_US);
        scl_hi();
        eeprom_delay_us(EEPROM_IO_US);
        if (sda_read()) {
            data |= 0x01u;
        }
        scl_lo();
        eeprom_delay_us(EEPROM_IO_US);
    }
    /* 主机应答位：最后字节发 NACK，其余发 ACK */
    sda_out();
    if (ack) {
        sda_lo();
    } else {
        sda_hi();
    }
    scl_hi();
    eeprom_delay_us(EEPROM_IO_US);
    scl_lo();
    eeprom_delay_us(EEPROM_IO_US);
    sda_in();
    return data;
}

/* 总线自恢复：释放 SDA 后给 9 个时钟脉冲，再发 STOP */
static void eeprom_bus_recover(void)
{
    sda_in();
    for (uint8_t i = 0; i < 9; i++) {
        scl_lo();
        eeprom_delay_us(EEPROM_IO_US);
        scl_hi();
        eeprom_delay_us(EEPROM_IO_US);
    }
    scl_lo();
    sda_out();
    iic_stop();
}

static int eeprom_page_write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    iic_start();
    if (iic_send_byte(EEPROM_WRITE_ADDR) != 0 ||
        iic_send_byte((uint8_t)addr) != 0) {
        iic_stop();
        return -1;
    }
    for (uint16_t i = 0; i < len; i++) {
        if (iic_send_byte(buf[i]) != 0) {
            iic_stop();
            return -1;
        }
    }
    iic_stop();
    return 0;
}

static int eeprom_read_raw(uint16_t addr, uint8_t *buf, uint16_t len)
{
    iic_start();
    if (iic_send_byte(EEPROM_WRITE_ADDR) != 0 ||
        iic_send_byte((uint8_t)addr) != 0) {
        iic_stop();
        return -1;
    }
    iic_start();                    /* 重复起始：切换为读 */
    if (iic_send_byte(EEPROM_READ_ADDR) != 0) {
        iic_stop();
        return -1;
    }
    for (uint16_t i = 0; i < len; i++) {
        buf[i] = iic_read_byte((i < len - 1) ? 1u : 0u);
    }
    iic_stop();
    return 0;
}

int BSP_EEPROM_Probe(void)
{
    int ok = -1;
    if (s_lock != NULL &&
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        iic_start();
        if (iic_send_byte(EEPROM_WRITE_ADDR) == 0) {
            ok = 0;
        }
        iic_stop();
        xSemaphoreGive(s_lock);
    }
    return ok;
}

int BSP_EEPROM_Init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = EEPROM_SCL_PIN | EEPROM_SDA_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;   /* 开漏 + 内部上拉（官方配置） */
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(EEPROM_SCL_PORT, &gpio);

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    scl_hi();
    sda_hi();
    return BSP_EEPROM_Probe();
}

int BSP_EEPROM_Read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    if (buf == NULL || (uint32_t)addr + len > BSP_EEPROM_SIZE) {
        return -1;
    }
    int ret = -1;
    if (s_lock != NULL &&
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        /* AT24C02 随机读支持跨页连续读（地址自动回绕） */
        if (eeprom_read_raw(addr, buf, len) == 0) {
            ret = 0;
        } else {
            eeprom_bus_recover();
        }
        xSemaphoreGive(s_lock);
    }
    return ret;
}

int BSP_EEPROM_Write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    if (buf == NULL || (uint32_t)addr + len > BSP_EEPROM_SIZE) {
        return -1;
    }
    int ret = -1;
    if (s_lock != NULL &&
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        uint16_t off = 0;
        while (off < len) {
            /* 页边界拆分：每页最多 8B */
            uint16_t page_left = BSP_EEPROM_PAGE -
                                 (uint16_t)((addr + off) % BSP_EEPROM_PAGE);
            uint16_t chunk = len - off;
            if (chunk > page_left) {
                chunk = page_left;
            }
            if (eeprom_page_write((uint16_t)(addr + off), buf + off,
                                  chunk) != 0) {
                eeprom_bus_recover();
                break;
            }
            off += chunk;
            osDelay(EEPROM_WRITE_CYCLE);   /* 等待内部写周期 */
        }
        if (off == len) {
            ret = 0;
        }
        xSemaphoreGive(s_lock);
    }
    return ret;
}
