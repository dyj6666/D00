/* ================================================================
 * I2C1 总线互斥实现（MPU6050 / EEPROM 共享总线安全）
 * ================================================================ */
#include "bsp_i2c.h"
#include "i2c.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t s_i2c1_mutex = NULL;
static SemaphoreHandle_t s_i2c2_mutex = NULL;

void BSP_I2C1_Init(void)
{
    if (s_i2c1_mutex == NULL) {
        s_i2c1_mutex = xSemaphoreCreateMutex();
    }
    /* 显式强制配置 I2C1 引脚（PB6=SCL/PB7=SDA，AF4 开漏上拉）。
     * 背景：HAL_I2C_MspInit / HAL_GPIO_Init 在部分构建产物中 AF 复用
     * 写入未生效（PB6/7 保持 AF0），导致 I2C1 信号未接入引脚、总线全
     * NACK。此处用寄存器级直接配置确保物理通路（不依赖 HAL）。 */
    GPIO_InitTypeDef g = {0};
    g.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    g.Mode = GPIO_MODE_AF_OD;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &g);
    /* 寄存器级兜底：直接写 AFRL（0x40020424，PB6/PB7 = AF4），绕过 HAL 潜在问题 */
    volatile uint32_t *afrl = (volatile uint32_t *)0x40020424u;
    *afrl = (*afrl & ~(0xFFUL << 24)) |
            ((uint32_t)GPIO_AF4_I2C1 << 24) |
            ((uint32_t)GPIO_AF4_I2C1 << 28);
    /* I2C 时序修复：实测 HAL_I2C_Init 配置的 CCR=0x0D/TRISE=0（异常），
     * 导致总线时钟远超 400kHz、传输 BERR。显式写入 400kHz 快速模式时序：
     *   CCR   = 0x8035（Fm，PCLK1=42MHz → 42M/(2*400k)≈53）
     *   TRISE = 43（42MHz × 1µs 上升时间 + 1） */
    *(volatile uint32_t *)0x4000541Cu = 0x8035u;
    *(volatile uint32_t *)0x40005420u = 43u;
}

int BSP_I2C1_Lock(uint32_t timeout_ms)
{
    if (s_i2c1_mutex == NULL) {
        return -1;
    }
    return (xSemaphoreTake(s_i2c1_mutex,
                           pdMS_TO_TICKS(timeout_ms)) == pdTRUE) ? 0 : -1;
}

void BSP_I2C1_Unlock(void)
{
    if (s_i2c1_mutex != NULL) {
        xSemaphoreGive(s_i2c1_mutex);
    }
}

/* I2C1 bus recovery: when a slave clamps SDA low waiting for clocks,
 * toggle SCL >=9 times to release, then restore AF open-drain + PE. */
void BSP_I2C1_BusRelease(void)
{
    __HAL_I2C_DISABLE(&hi2c1);

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;   /* PB6=SCL PB7=SDA */
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6 | GPIO_PIN_7, GPIO_PIN_SET);
    for (int i = 0; i < 9; i++) {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_RESET);
        for (volatile uint32_t d = 0; d < 1000u; d++) { }  /* ~6us */
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
        for (volatile uint32_t d = 0; d < 1000u; d++) { }
    }
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);   /* SCL idle */

    (void)HAL_I2C_Init(&hi2c1);   /* restore AF open-drain + PE */
}

void BSP_I2C2_Init(void)
{
    if (s_i2c2_mutex == NULL) {
        s_i2c2_mutex = xSemaphoreCreateMutex();
    }
}

int BSP_I2C2_Lock(uint32_t timeout_ms)
{
    if (s_i2c2_mutex == NULL) {
        return -1;
    }
    return (xSemaphoreTake(s_i2c2_mutex,
                           pdMS_TO_TICKS(timeout_ms)) == pdTRUE) ? 0 : -1;
}

void BSP_I2C2_Unlock(void)
{
    if (s_i2c2_mutex != NULL) {
        xSemaphoreGive(s_i2c2_mutex);
    }
}
