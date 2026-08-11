/* ================================================================
 * bsp_rtc —— RTC 抽象：时间读写与备份域
 *
 * 架构位置：APP BSP 层；SNTP 校准与时间戳
 * ================================================================ */
#include "bsp_rtc.h"
#include "rtc.h"
#include "stm32f4xx_hal.h"

void BSP_RTC_WriteBackupReg(uint32_t index, uint32_t value)
{
    HAL_PWR_EnableBkUpAccess();
    /* HAL_RTCEx_BKUPWrite 第二参数为寄存器索引 0..19（非地址！） */
    HAL_RTCEx_BKUPWrite(&hrtc, index, value);
}

uint32_t BSP_RTC_ReadBackupReg(uint32_t index)
{
    return HAL_RTCEx_BKUPRead(&hrtc, index);
}
