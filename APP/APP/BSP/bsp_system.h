#ifndef BSP_SYSTEM_H
#define BSP_SYSTEM_H

#include <stdint.h>

/* 复位原因枚举 */
typedef enum {
    BSP_RESET_UNKNOWN = 0,
    BSP_RESET_POWER_ON,
    BSP_RESET_PIN,
    BSP_RESET_SOFTWARE,
    BSP_RESET_IWDG,
    BSP_RESET_WWDG
} bsp_reset_reason_t;

/* 软件复位系统 */
void BSP_SystemReset(void);

/* 阻塞延时（毫秒）。可在任务上下文调用。 */
void BSP_DelayMs(uint32_t ms);

/* 获取本次上电的复位原因（读取后自动清除标志） */
bsp_reset_reason_t BSP_GetResetReason(void);

#endif
