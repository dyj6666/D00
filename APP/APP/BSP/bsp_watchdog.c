/* 平台实现：STM32F4xx IWDG */
#include "bsp_watchdog.h"
#include "iwdg.h"
#include "app_config.h"

void BSP_Watchdog_Refresh(void)
{
#if APP_DEBUG_MODE
    /* 调试构建：IWDG 未启动，无需喂狗 */
#else
    HAL_IWDG_Refresh(&hiwdg);
#endif
}
