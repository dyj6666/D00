/* ================================================================
 * bsp_rtc —— RTC 抽象：时间读写与备份域访问
 *
 * 架构位置：APP BSP 层；SNTP 校准与时间戳取用经本层
 * ================================================================ */
#ifndef BSP_RTC_H
#define BSP_RTC_H

#include <stdint.h>

/* 写备份寄存器（index: 0 起，数量取决于平台）。
 * 备份寄存器在系统复位/待机后保持，用于跨复位传递标志。 */
void BSP_RTC_WriteBackupReg(uint32_t index, uint32_t value);

/* 读备份寄存器 */
uint32_t BSP_RTC_ReadBackupReg(uint32_t index);

/* 与 HAL 解耦的日期时间结构（供服务层使用，避免 HAL 类型上溢） */
typedef struct {
    uint8_t  hours, minutes, seconds;
    uint8_t  day, month;
    uint16_t year;      /* 完整年份，如 2026 */
    uint8_t  weekday;   /* 1=周一 .. 7=周日 */
} bsp_rtc_datetime_t;

/* 设置 RTC 日期时间（BIN 格式）；返回 0 成功 */
int BSP_RTC_SetDateTime(const bsp_rtc_datetime_t *dt);

/* 读取 RTC 日期时间；返回 0 成功 */
int BSP_RTC_GetDateTime(bsp_rtc_datetime_t *dt);

#endif
