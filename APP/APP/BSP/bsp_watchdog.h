/* ================================================================
 * bsp_watchdog —— 硬件看门狗抽象：启动与喂狗
 *
 * 架构位置：APP BSP 层；与任务级 watchdog 构成双层防线
 * ================================================================ */
#ifndef BSP_WATCHDOG_H
#define BSP_WATCHDOG_H

/* 刷新硬件看门狗（IWDG）。必须在超时前周期性调用，否则系统复位。 */
void BSP_Watchdog_Refresh(void);

#endif
