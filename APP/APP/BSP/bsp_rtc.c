/* 平台实现：STM32F4xx RTC 备份寄存器 */
#include "bsp_rtc.h"
#include "rtc.h"
#include "stm32f4xx_hal.h"

void BSP_RTC_WriteBackupReg(uint32_t index, uint32_t value)
{
    HAL_PWR_EnableBkUpAccess();
    HAL_RTCEx_BKUPWrite(&hrtc, (uint32_t)(RTC_BKP_DR1 + (index * 4)), value);
}

uint32_t BSP_RTC_ReadBackupReg(uint32_t index)
{
    return HAL_RTCEx_BKUPRead(&hrtc, (uint32_t)(RTC_BKP_DR1 + (index * 4)));
}
