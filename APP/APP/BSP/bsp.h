/* 板级支持包（Board Support Package）
 *
 * 所有平台相关能力都收敛到本层：
 *   - bsp_system : 系统复位 / 延时 / 复位原因
 *   - bsp_uart   : DMA 串口收发（调试口 + 上位机口）
 *   - bsp_watchdog : 硬件看门狗
 *   - bsp_rtc    : 备份寄存器（掉电保持标志位）
 *
 * 应用层与服务层只允许通过 BSP 接口访问硬件，
 * 移植到其他平台时只需重写 BSP 目录下的实现文件。
 */
/* ================================================================
 * bsp —— 板级支持包总接口：系统/GPIO/UART/IWDG/RTC/EEPROM
 *
 * 架构位置：APP BSP 层；服务层与应用层只经本层访问硬件，便于移植
 * ================================================================ */
#ifndef BSP_H
#define BSP_H

#include "bsp_system.h"
#include "bsp_uart.h"
#include "bsp_watchdog.h"
#include "bsp_rtc.h"
#include "bsp_gpio.h"

#endif
