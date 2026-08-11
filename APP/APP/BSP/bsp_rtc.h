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

#endif
