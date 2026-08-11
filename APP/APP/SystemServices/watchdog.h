/* ================================================================
 * watchdog —— 任务级看门狗：模块心跳监控 + 复位处置
 *
 * 架构位置：APP 服务层；与硬件 IWDG 构成双层防线
 * ================================================================ */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "FreeRTOS.h"
#include "task.h"

#include <stdint.h>

#define WDOG_MAX_TASKS 8

void WDOG_Init(void);

/* 注册需要监控的任务。handle 为该任务句柄，timeout_ms 为允许的最大静默时间。
 * 重复注册同一任务时更新超时参数。返回 0 成功。 */
int  WDOG_RegisterTask(const char *name, TaskHandle_t handle, uint32_t timeout_ms);

/* 任务在周期循环中调用，报告“我还活着”。 */
void WDOG_Kick(TaskHandle_t handle);

/* 打印监控状态（sysmon 使用） */
void WDOG_PrintStatus(void);

#endif
