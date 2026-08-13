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

int BSP_RTC_SetDateTime(const bsp_rtc_datetime_t *dt)
{
    if (dt == NULL) return -1;
    RTC_TimeTypeDef t;
    t.Hours = dt->hours;
    t.Minutes = dt->minutes;
    t.Seconds = dt->seconds;
    t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    t.StoreOperation = RTC_STOREOPERATION_RESET;
    RTC_DateTypeDef d;
    d.Year = (uint8_t)(dt->year % 100u);
    d.Month = dt->month;
    d.Date = dt->day;
    d.WeekDay = dt->weekday;
    if (HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) return -2;
    if (HAL_RTC_SetDate(&hrtc, &d, RTC_FORMAT_BIN) != HAL_OK) return -3;
    return 0;
}

int BSP_RTC_GetDateTime(bsp_rtc_datetime_t *dt)
{
    if (dt == NULL) return -1;
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;
    if (HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) return -2;
    if (HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN) != HAL_OK) return -3;
    dt->hours = t.Hours;
    dt->minutes = t.Minutes;
    dt->seconds = t.Seconds;
    dt->day = d.Date;
    dt->month = d.Month;
    dt->year = (uint16_t)(2000u + d.Year);
    dt->weekday = d.WeekDay;
    return 0;
}
