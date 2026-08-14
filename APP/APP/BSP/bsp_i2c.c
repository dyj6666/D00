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
