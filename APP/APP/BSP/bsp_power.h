/* ================================================================
 * bsp_power —— 低功耗管理：WFI 空闲 + 可选 STOP Tickless
 *
 * 架构位置：APP BSP 层；供内核空闲路径与 shell `power` 命令调用
 *
 * 两级省电设计（工业默认安全）：
 *   1) WFI 空闲钩子（常开）：CPU 空闲时停止旋转，等下一个中断
 *      （SysTick 1ms）唤醒——零外设影响，纯省 CPU 功耗；
 *   2) STOP Tickless（`power on` 可选）：内核全阻塞且空闲 >2s 时
 *      进入 STOP，RTC 唤醒定时器按时长唤醒，IWDG 安全窗口内
 *      补 tick。注意：休眠期间 CAN/ETH/UART 数据不接收，仅适合
 *      低功耗场景临时开启（IMU 200Hz 运行期间自动不触发）。
 * ================================================================ */
#ifndef BSP_POWER_H
#define BSP_POWER_H

#include <stdint.h>

void  BSP_Power_Init(void);
uint8_t BSP_Power_IsEnabled(void);
int   BSP_Power_Enable(void);
void  BSP_Power_Disable(void);

/* 内核空闲路径（portSUPPRESS_TICKS_AND_SLEEP）：禁止任务上下文调用 */
void  BSP_Power_TicklessSleep(uint32_t xExpectedIdleTime);

#endif /* BSP_POWER_H */
