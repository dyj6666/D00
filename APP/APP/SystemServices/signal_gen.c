#include "signal_gen.h"

#include <string.h>

#include "cmsis_os2.h"
#include "stm32f4xx_hal.h"

#define SG_STACK_SIZE  512
#define SG_FLAG_STOP   0x01

typedef enum {
    SG_MODE_NONE = 0,
    SG_MODE_UART,
    SG_MODE_SPI,
    SG_MODE_I2C,
    SG_MODE_I2C_COMPLEX
} sg_mode_t;

static UART_HandleTypeDef sg_huart6;
static DMA_HandleTypeDef  sg_hdma_usart6_tx;
static osThreadId_t       sg_task_id = NULL;
static volatile uint8_t   sg_running = 0;
static volatile sg_mode_t sg_mode = SG_MODE_NONE;
static uint8_t            sg_data[SG_TEXT_MAX];
static uint16_t           sg_len = 0;
static uint16_t           sg_interval_ms = 5;
static uint16_t           sg_i2c_addr = 0x50 << 1;
static volatile uint8_t   sg_uart_done = 0;  /* USART6 TX DMA 完成标志 */

/* 软件 SPI（模式0）输出：SCK=PE5 / MOSI=PE6 / CS=PF6。
 * 全部为独立引脚，与板载 LCD（占用 PB15/FSMC）零冲突。 */
#define SG_SPI_SCK_PORT  GPIOE
#define SG_SPI_SCK_PIN   GPIO_PIN_5
#define SG_SPI_MOSI_PORT GPIOE
#define SG_SPI_MOSI_PIN  GPIO_PIN_6
#define SG_SPI_CS_PORT   GPIOF
#define SG_SPI_CS_PIN    GPIO_PIN_6

/* 软件模拟 I2C（bit-bang）：PE2=SCL / PE3=SDA（按键引脚，独立引出） */
#define SG_I2C_SCL_PORT  GPIOE
#define SG_I2C_SCL_PIN   GPIO_PIN_2
#define SG_I2C_SDA_PORT  GPIOE
#define SG_I2C_SDA_PIN   GPIO_PIN_3

static void sg_i2c_delay_half(void)
{
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < 840u) {  /* ~5us @168MHz => SCL 100kHz */
    }
}

static void sg_i2c_scl_high(void)
{
    HAL_GPIO_WritePin(SG_I2C_SCL_PORT, SG_I2C_SCL_PIN, GPIO_PIN_SET);
    sg_i2c_delay_half();
}

static void sg_i2c_scl_low(void)
{
    HAL_GPIO_WritePin(SG_I2C_SCL_PORT, SG_I2C_SCL_PIN, GPIO_PIN_RESET);
    sg_i2c_delay_half();
}

static void sg_i2c_sda(uint8_t v)
{
    HAL_GPIO_WritePin(SG_I2C_SDA_PORT, SG_I2C_SDA_PIN,
                      v ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void sg_i2c_start(void)
{
    sg_i2c_sda(1);
    sg_i2c_scl_high();
    sg_i2c_sda(0);
    sg_i2c_scl_low();
}

static void sg_i2c_stop(void)
{
    sg_i2c_sda(0);
    sg_i2c_scl_high();
    sg_i2c_sda(1);
}

static void sg_i2c_write_byte(uint8_t byte, uint8_t ack_bit)
{
    for (int b = 7; b >= 0; b--) {
        sg_i2c_sda((byte >> b) & 1);
        sg_i2c_delay_half();   /* 等 SDA 稳定后再拉高 SCL，避免边沿混叠 */
        sg_i2c_scl_high();
        sg_i2c_scl_low();
    }
    /* ACK 位：ack_bit=1 模拟从机应答（SDA 拉低），0 => NACK（SDA 高） */
    sg_i2c_sda(ack_bit ? 0 : 1);
    sg_i2c_delay_half();
    sg_i2c_scl_high();
    sg_i2c_scl_low();
}

static void sg_i2c_repeated_start(void)
{
    sg_i2c_sda(1);            /* 释放 SDA（SCL 低） */
    sg_i2c_delay_half();
    sg_i2c_scl_high();
    sg_i2c_sda(0);            /* SCL 高时 SDA 下降 = 重复起始 */
    sg_i2c_delay_half();
    sg_i2c_scl_low();
}

static void sg_i2c_tx_frame(uint8_t addr7, const uint8_t *data, uint16_t len)
{
    sg_i2c_start();
    sg_i2c_write_byte((uint8_t)(addr7 << 1), 1);
    for (uint16_t i = 0; i < len; i++) {
        sg_i2c_write_byte(data[i], 1);
    }
    sg_i2c_stop();
}

/* 复杂演示帧：写3字节+ACK → 重复起始 → 读地址+ACK → 数据+NACK → STOP */
static void sg_i2c_tx_complex(uint8_t addr7)
{
    sg_i2c_start();
    sg_i2c_write_byte((uint8_t)(addr7 << 1), 1);
    sg_i2c_write_byte(0xAA, 1);
    sg_i2c_write_byte(0x55, 1);
    sg_i2c_write_byte(0x01, 1);
    sg_i2c_repeated_start();
    sg_i2c_write_byte((uint8_t)((addr7 << 1) | 1), 1);
    sg_i2c_write_byte(0x02, 0);   /* 最后数据 NACK */
    sg_i2c_stop();
}

static void sg_i2c_init_gpio(void)
{
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /* 使能 DWT 周期计数器（软件 I2C 微秒延时） */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* PE2=SCL 推挽输出；PE3=SDA 开漏输出 + 上拉（读 ACK 可释放） */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = SG_I2C_SCL_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(SG_I2C_SCL_PORT, &gpio);

    gpio.Pin = SG_I2C_SDA_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(SG_I2C_SDA_PORT, &gpio);
    sg_i2c_sda(1);
}

/* 软件 SPI（模式0：CPOL=0/CPHA=0，MSB 先），无固定限速（GPIO 直驱极速） */
static void sg_spi_write_bytes(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int b = 7; b >= 0; b--) {
            HAL_GPIO_WritePin(SG_SPI_SCK_PORT, SG_SPI_SCK_PIN, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(SG_SPI_MOSI_PORT, SG_SPI_MOSI_PIN,
                              (byte & (1u << b)) ? GPIO_PIN_SET : GPIO_PIN_RESET);
            HAL_GPIO_WritePin(SG_SPI_SCK_PORT, SG_SPI_SCK_PIN, GPIO_PIN_SET);
        }
    }
    HAL_GPIO_WritePin(SG_SPI_SCK_PORT, SG_SPI_SCK_PIN, GPIO_PIN_RESET);
}

static void sg_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (sg_mode == SG_MODE_UART) {
            sg_uart_done = 0;
            if (HAL_UART_Transmit_DMA(&sg_huart6, sg_data, sg_len) == HAL_OK) {
                while (!sg_uart_done) {
                    osDelay(1);
                }
            }
        } else if (sg_mode == SG_MODE_SPI) {
            HAL_GPIO_WritePin(SG_SPI_CS_PORT, SG_SPI_CS_PIN, GPIO_PIN_RESET);
            sg_spi_write_bytes(sg_data, sg_len);
            HAL_GPIO_WritePin(SG_SPI_CS_PORT, SG_SPI_CS_PIN, GPIO_PIN_SET);
        } else if (sg_mode == SG_MODE_I2C) {
            sg_i2c_tx_frame((uint8_t)(sg_i2c_addr >> 1), sg_data, sg_len);
        } else if (sg_mode == SG_MODE_I2C_COMPLEX) {
            sg_i2c_tx_complex((uint8_t)(sg_i2c_addr >> 1));
        } else {
            break;
        }
        uint32_t flags = osThreadFlagsWait(SG_FLAG_STOP, osFlagsWaitAny,
                                           sg_interval_ms);
        if (flags & SG_FLAG_STOP) {
            break;
        }
    }
    sg_mode = SG_MODE_NONE;
    sg_running = 0;
    osThreadExit();
}

static int sg_start_common(uint32_t baud, uint16_t interval_ms)
{
    if (baud < SG_BAUD_MIN || baud > SG_BAUD_MAX) {
        return -1;
    }
    if (sg_running) {
        SG_UartStop();
    }

    __HAL_RCC_USART6_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* PC6 -> USART6_TX (AF8)，覆盖 TIM8_CH1 的 PWM 复用 */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_6;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF8_USART6;
    HAL_GPIO_Init(GPIOC, &gpio);

    memset(&sg_huart6, 0, sizeof(sg_huart6));
    sg_huart6.Instance = USART6;
    sg_huart6.Init.BaudRate = baud;
    sg_huart6.Init.WordLength = UART_WORDLENGTH_8B;
    sg_huart6.Init.StopBits = UART_STOPBITS_1;
    sg_huart6.Init.Parity = UART_PARITY_NONE;
    sg_huart6.Init.Mode = UART_MODE_TX;
    sg_huart6.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    sg_huart6.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&sg_huart6) != HAL_OK) {
        __HAL_RCC_USART6_CLK_DISABLE();
        return -2;
    }

    /* USART6_TX = DMA2_Stream6/Channel5：DMA 传输期间任务睡眠，CPU 近零占用 */
    __HAL_RCC_DMA2_CLK_ENABLE();
    memset(&sg_hdma_usart6_tx, 0, sizeof(sg_hdma_usart6_tx));
    sg_hdma_usart6_tx.Instance = DMA2_Stream6;
    sg_hdma_usart6_tx.Init.Channel = DMA_CHANNEL_5;
    sg_hdma_usart6_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    sg_hdma_usart6_tx.Init.PeriphInc = DMA_PINC_DISABLE;
    sg_hdma_usart6_tx.Init.MemInc = DMA_MINC_ENABLE;
    sg_hdma_usart6_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    sg_hdma_usart6_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    sg_hdma_usart6_tx.Init.Mode = DMA_NORMAL;
    sg_hdma_usart6_tx.Init.Priority = DMA_PRIORITY_LOW;
    sg_hdma_usart6_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&sg_hdma_usart6_tx) != HAL_OK) {
        HAL_UART_DeInit(&sg_huart6);
        __HAL_RCC_USART6_CLK_DISABLE();
        return -2;
    }
    __HAL_LINKDMA(&sg_huart6, hdmatx, sg_hdma_usart6_tx);
    HAL_NVIC_SetPriority(DMA2_Stream6_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream6_IRQn);

    sg_interval_ms = interval_ms;
    sg_running = 1;

    osThreadAttr_t attr = {
        .name = "SG_UART",
        .stack_size = SG_STACK_SIZE,
        .priority = osPriorityLow,
    };
    sg_task_id = osThreadNew(sg_tx_task, NULL, &attr);
    if (sg_task_id == NULL) {
        sg_running = 0;
        HAL_UART_DeInit(&sg_huart6);
        __HAL_RCC_USART6_CLK_DISABLE();
        return -3;
    }
    return 0;
}

static int sg_hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int SG_UartStart(uint32_t baud, const char *text, uint16_t interval_ms)
{
    size_t len;
    if (text == NULL) return -1;
    len = strlen(text);
    if (len == 0 || len >= SG_TEXT_MAX) return -1;
    memcpy(sg_data, text, len);
    sg_len = (uint16_t)len;
    sg_mode = SG_MODE_UART;
    return sg_start_common(baud, interval_ms);
}

int SG_UartStartHex(uint32_t baud, const char *hex, uint16_t interval_ms)
{
    size_t len;
    if (hex == NULL) return -1;
    len = strlen(hex);
    if (len == 0 || len % 2 != 0 || len / 2 >= SG_TEXT_MAX) return -1;
    for (size_t i = 0; i < len / 2; i++) {
        int hi = sg_hex_value(hex[i * 2]);
        int lo = sg_hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        sg_data[i] = (uint8_t)((hi << 4) | lo);
    }
    sg_len = (uint16_t)(len / 2);
    sg_mode = SG_MODE_UART;
    return sg_start_common(baud, interval_ms);
}

static int sg_spi_start_bytes(const uint8_t *data, uint16_t len,
                              uint16_t interval_ms)
{
    if (data == NULL || len == 0 || len > SG_TEXT_MAX) return -1;
    if (sg_running) {
        SG_UartStop();
    }

    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    /* 软件 SPI：SCK=PE5 / MOSI=PE6（推挽输出，高速） */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = SG_SPI_SCK_PIN | SG_SPI_MOSI_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(SG_SPI_SCK_PORT, &gpio);

    /* CS=PF6，低有效，空闲高 */
    gpio.Pin = SG_SPI_CS_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(SG_SPI_CS_PORT, &gpio);
    HAL_GPIO_WritePin(SG_SPI_CS_PORT, SG_SPI_CS_PIN, GPIO_PIN_SET);

    memcpy(sg_data, data, len);
    sg_len = len;
    sg_interval_ms = interval_ms;
    sg_mode = SG_MODE_SPI;
    sg_running = 1;

    osThreadAttr_t attr = {
        .name = "SG_SPI",
        .stack_size = SG_STACK_SIZE,
        .priority = osPriorityLow,
    };
    sg_task_id = osThreadNew(sg_tx_task, NULL, &attr);
    if (sg_task_id == NULL) {
        sg_mode = SG_MODE_NONE;
        sg_running = 0;
        return -3;
    }
    return 0;
}

int SG_SpiStartHex(const char *hex, uint16_t interval_ms)
{
    size_t len;
    uint8_t buf[SG_TEXT_MAX];
    if (hex == NULL) return -1;
    len = strlen(hex);
    if (len == 0 || len % 2 != 0 || len / 2 > SG_TEXT_MAX) return -1;
    for (size_t i = 0; i < len / 2; i++) {
        int hi = sg_hex_value(hex[i * 2]);
        int lo = sg_hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    return sg_spi_start_bytes(buf, (uint16_t)(len / 2), interval_ms);
}

void SG_SpiStop(void)
{
    if (!sg_running && sg_task_id == NULL) {
        return;
    }
    if (sg_task_id != NULL) {
        osThreadFlagsSet(sg_task_id, SG_FLAG_STOP);
        sg_task_id = NULL;
    }
    osDelay(20);
}

int SG_I2CStart(uint8_t addr, const char *hex, uint16_t interval_ms)
{
    size_t len;
    uint8_t buf[SG_TEXT_MAX];
    if (hex == NULL) return -1;
    len = strlen(hex);
    if (len == 0 || len % 2 != 0 || len / 2 > SG_TEXT_MAX) return -1;
    for (size_t i = 0; i < len / 2; i++) {
        int hi = sg_hex_value(hex[i * 2]);
        int lo = sg_hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    if (sg_running) {
        SG_UartStop();
    }

    sg_i2c_init_gpio();

    memcpy(sg_data, buf, len / 2);
    sg_len = (uint16_t)(len / 2);
    sg_i2c_addr = (uint16_t)(addr << 1);
    sg_interval_ms = interval_ms;
    sg_mode = SG_MODE_I2C;
    sg_running = 1;

    osThreadAttr_t attr = {
        .name = "SG_I2C",
        .stack_size = SG_STACK_SIZE,
        .priority = osPriorityHigh,   /* bit-bang 时序需避免被任务抢占 */
    };
    sg_task_id = osThreadNew(sg_tx_task, NULL, &attr);
    if (sg_task_id == NULL) {
        sg_mode = SG_MODE_NONE;
        sg_running = 0;
        return -3;
    }
    return 0;
}

void SG_I2CStop(void)
{
    if (!sg_running && sg_task_id == NULL) {
        return;
    }
    if (sg_task_id != NULL) {
        osThreadFlagsSet(sg_task_id, SG_FLAG_STOP);
        sg_task_id = NULL;
    }
    osDelay(20);
}

int SG_I2CComplexStart(uint8_t addr, uint16_t interval_ms)
{
    if (sg_running) {
        SG_UartStop();
    }
    sg_i2c_init_gpio();
    sg_i2c_addr = (uint16_t)(addr << 1);
    sg_interval_ms = interval_ms;
    sg_mode = SG_MODE_I2C_COMPLEX;
    sg_running = 1;

    osThreadAttr_t attr = {
        .name = "SG_I2C",
        .stack_size = SG_STACK_SIZE,
        .priority = osPriorityHigh,
    };
    sg_task_id = osThreadNew(sg_tx_task, NULL, &attr);
    if (sg_task_id == NULL) {
        sg_mode = SG_MODE_NONE;
        sg_running = 0;
        return -3;
    }
    return 0;
}

void SG_UartStop(void)
{
    if (!sg_running && sg_task_id == NULL) {
        return;
    }
    sg_uart_done = 1;
    HAL_UART_DMAStop(&sg_huart6);
    if (sg_task_id != NULL) {
        osThreadFlagsSet(sg_task_id, SG_FLAG_STOP);
        sg_task_id = NULL;
    }
    /* 等待任务退出当前发送帧（< 5ms @ 115200/64B），避免与后续 Start 竞态 */
    osDelay(20);
}

uint8_t SG_UartIsRunning(void)
{
    return sg_running;
}

/* USART6 TX DMA 完成通知（bsp_uart 弱钩子，非 BSP 通道） */
void BSP_UART_OnTxComplete(void)
{
    sg_uart_done = 1;
}

/* DMA2_Stream6 中断转发（stm32f4xx_it.c 调用） */
void SG_Uart_DMA_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&sg_hdma_usart6_tx);
}
