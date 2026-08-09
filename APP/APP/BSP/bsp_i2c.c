/* ================================================================
 * I2C1 总线互斥实现（MPU6050 / EEPROM 共享总线安全）
 * ================================================================ */
#include "bsp_i2c.h"
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
