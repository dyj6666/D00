/* ================================================================
 * bsp_es8388 —— ES8388 音频 Codec 驱动实现
 *
 * 移植自正点原子探索者 F407 例程（实验43 音乐播放器实验 es8388.c），
 * 软 I2C 原语内嵌（与 bsp_eeprom 同总线时序，PB8/PB9），独立互斥锁。
 *
 * 注意：与 AT24C02 共享 PB8/PB9 软 I2C 总线——两驱动各自持锁，
 * 事务均为微秒级，低频访问下竞争窗口可忽略；如需强互斥可后续
 * 抽公共总线锁。
 * ================================================================ */
#include "bsp_es8388.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "stm32f4xx_hal.h"

#define ES8388_ADDR     0x10u        /* 器件地址（7 位，探索者固定） */

/* ---------------- 软 I2C 引脚（与 EEPROM 共用总线） ---------------- */
#define ES_SCL_PORT     GPIOB
#define ES_SCL_PIN      GPIO_PIN_8
#define ES_SDA_PORT     GPIOB
#define ES_SDA_PIN      GPIO_PIN_9
#define ES_SDA_MODE_POS 18u           /* PB9 MODER 位偏移 */

#define ES_IO_US        2u            /* 字节内位时序半周期（~250kHz） */
#define ES_EDGE_US      4u            /* 起始/停止建立保持 */

static SemaphoreHandle_t s_lock;

/* ---------------- 微秒延时（DWT，带迭代上限防死循环） ---------------- */
static void es_delay_us(uint32_t us)
{
    uint32_t cyc = us * 168u;
    uint32_t guard = cyc * 4u + 1000u;
    uint32_t t0 = DWT->CYCCNT;
    while (((DWT->CYCCNT - t0) < cyc) && (guard-- > 0u)) {
    }
}

/* ---------------- 引脚原语 ---------------- */
static void scl_hi(void) { HAL_GPIO_WritePin(ES_SCL_PORT, ES_SCL_PIN, GPIO_PIN_SET); }
static void scl_lo(void) { HAL_GPIO_WritePin(ES_SCL_PORT, ES_SCL_PIN, GPIO_PIN_RESET); }
static void sda_hi(void) { HAL_GPIO_WritePin(ES_SDA_PORT, ES_SDA_PIN, GPIO_PIN_SET); }
static void sda_lo(void) { HAL_GPIO_WritePin(ES_SDA_PORT, ES_SDA_PIN, GPIO_PIN_RESET); }

static void sda_out(void)
{
    GPIOB->MODER = (GPIOB->MODER & ~(3u << ES_SDA_MODE_POS)) |
                   (1u << ES_SDA_MODE_POS);
}

static void sda_in(void)
{
    GPIOB->MODER = (GPIOB->MODER & ~(3u << ES_SDA_MODE_POS));
}

static uint8_t sda_read(void)
{
    return (HAL_GPIO_ReadPin(ES_SDA_PORT, ES_SDA_PIN) == GPIO_PIN_SET) ? 1u : 0u;
}

/* ---------------- 软 I2C 时序（与 bsp_eeprom 同款） ---------------- */
static void iic_start(void)
{
    sda_out();
    sda_hi();
    scl_hi();
    es_delay_us(ES_EDGE_US);
    sda_lo();
    es_delay_us(ES_EDGE_US);
    scl_lo();
}

static void iic_stop(void)
{
    sda_out();
    sda_lo();
    scl_hi();
    es_delay_us(ES_EDGE_US);
    sda_hi();
    es_delay_us(ES_EDGE_US);
    scl_lo();
}

static int iic_wait_ack(void)
{
    uint32_t t = 0;
    sda_in();
    scl_hi();
    es_delay_us(ES_IO_US);
    while (sda_read() && t < 500u) {
        es_delay_us(1u);
        t++;
    }
    scl_lo();
    es_delay_us(ES_IO_US);
    sda_out();
    return (t < 500u) ? 0 : -1;
}

static int iic_send_byte(uint8_t data)
{
    sda_out();
    scl_lo();
    for (uint8_t i = 0; i < 8u; i++) {
        if (data & 0x80u) {
            sda_hi();
        } else {
            sda_lo();
        }
        data <<= 1;
        es_delay_us(ES_IO_US);
        scl_hi();
        es_delay_us(ES_IO_US);
        scl_lo();
        es_delay_us(ES_IO_US);
    }
    return iic_wait_ack();
}

static uint8_t iic_read_byte(uint8_t ack)
{
    uint8_t data = 0;
    sda_in();
    for (uint8_t i = 0; i < 8u; i++) {
        data <<= 1;
        es_delay_us(ES_IO_US);
        scl_hi();
        es_delay_us(ES_IO_US);
        if (sda_read()) {
            data |= 0x01u;
        }
        scl_lo();
        es_delay_us(ES_IO_US);
    }
    sda_out();
    if (ack) {
        sda_lo();
    } else {
        sda_hi();
    }
    scl_hi();
    es_delay_us(ES_IO_US);
    scl_lo();
    es_delay_us(ES_IO_US);
    sda_in();
    return data;
}

/* ---------------- 对外接口 ---------------- */

uint8_t BSP_ES8388_WriteReg(uint8_t reg, uint8_t val)
{
    uint8_t ret = 1;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        iic_start();
        if (iic_send_byte((uint8_t)(ES8388_ADDR << 1)) == 0 &&   /* 写地址 */
            iic_send_byte(reg) == 0 &&
            iic_send_byte(val) == 0) {
            ret = 0;
        }
        iic_stop();
        xSemaphoreGive(s_lock);
    }
    return ret;
}

uint8_t BSP_ES8388_ReadReg(uint8_t reg)
{
    uint8_t temp = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        iic_start();
        if (iic_send_byte((uint8_t)(ES8388_ADDR << 1)) == 0 &&
            iic_send_byte(reg) == 0) {
            iic_start();
            if (iic_send_byte((uint8_t)((ES8388_ADDR << 1) | 1u)) == 0) {
                temp = iic_read_byte(0);
            }
        }
        iic_stop();
        xSemaphoreGive(s_lock);
    }
    return temp;
}

void BSP_ES8388_AddaCfg(uint8_t dacen, uint8_t adcen)
{
    uint8_t tempreg = 0;
    tempreg |= (uint8_t)((!dacen) << 0);
    tempreg |= (uint8_t)((!adcen) << 1);
    tempreg |= (uint8_t)((!dacen) << 2);
    tempreg |= (uint8_t)((!adcen) << 3);
    BSP_ES8388_WriteReg(0x02, tempreg);
}

void BSP_ES8388_OutputCfg(uint8_t o1en, uint8_t o2en)
{
    uint8_t tempreg = 0;
    tempreg |= (uint8_t)(o1en ? (3u << 4) : 0u);
    tempreg |= (uint8_t)(o2en ? (3u << 2) : 0u);
    BSP_ES8388_WriteReg(0x04, tempreg);
}

void BSP_ES8388_I2sCfg(uint8_t fmt, uint8_t len)
{
    fmt &= 0x03u;
    len &= 0x07u;
    BSP_ES8388_WriteReg(23, (uint8_t)((fmt << 1) | (len << 3)));
}

void BSP_ES8388_SpkVolSet(uint8_t volume)
{
    if (volume > 33u) {
        volume = 33u;
    }
    BSP_ES8388_WriteReg(0x30, volume);   /* 喇叭 L */
    BSP_ES8388_WriteReg(0x31, volume);   /* 喇叭 R */
}

void BSP_ES8388_HpVolSet(uint8_t volume)
{
    if (volume > 33u) {
        volume = 33u;
    }
    BSP_ES8388_WriteReg(0x2E, volume);   /* 耳机 L */
    BSP_ES8388_WriteReg(0x2F, volume);   /* 耳机 R */
}

uint8_t BSP_ES8388_Init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock == NULL) {
        return 1;
    }

    /* 软 I2C GPIO：开漏 + 上拉（与 EEPROM 同配置） */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = ES_SCL_PIN | ES_SDA_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* DWT 使能（es_delay_us 依赖；幂等） */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    BSP_ES8388_WriteReg(0, 0x80);        /* 软复位 */
    BSP_ES8388_WriteReg(0, 0x00);
    HAL_Delay(100);

    BSP_ES8388_WriteReg(0x01, 0x58);
    BSP_ES8388_WriteReg(0x01, 0x50);
    BSP_ES8388_WriteReg(0x02, 0xF3);
    BSP_ES8388_WriteReg(0x02, 0xF0);

    BSP_ES8388_WriteReg(0x03, 0x09);     /* 麦克风偏置关闭 */
    BSP_ES8388_WriteReg(0x00, 0x06);     /* 参考/500K 驱动使能 */
    BSP_ES8388_WriteReg(0x04, 0x00);     /* DAC 电源管理（通道由 OutputCfg 开） */
    BSP_ES8388_WriteReg(0x08, 0x00);     /* MCLK 不分频 */
    BSP_ES8388_WriteReg(0x2B, 0x80);     /* DACLRC 与 ADCLRC 相同 */

    BSP_ES8388_WriteReg(0x09, 0x88);     /* ADC PGA +24dB（播放不用） */
    BSP_ES8388_WriteReg(0x0C, 0x4C);     /* ADC 数据选择 16bit */
    BSP_ES8388_WriteReg(0x0D, 0x02);     /* ADC MCLK/采样率=256 */
    BSP_ES8388_WriteReg(0x10, 0x00);     /* ADC 数字音量最小 */
    BSP_ES8388_WriteReg(0x11, 0x00);

    BSP_ES8388_WriteReg(0x17, 0x18);     /* DAC 音频数据 16bit */
    BSP_ES8388_WriteReg(0x18, 0x02);     /* DAC MCLK/采样率=256 */
    BSP_ES8388_WriteReg(0x1A, 0x00);     /* DAC 数字音量最小（音量走 0x30/31） */
    BSP_ES8388_WriteReg(0x1B, 0x00);
    BSP_ES8388_WriteReg(0x27, 0xB8);     /* L 混频器 */
    BSP_ES8388_WriteReg(0x2A, 0xB8);     /* R 混频器 */

    return 0;
}
