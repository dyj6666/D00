/* ================================================================
 * bsp_watchdog —— 硬件看门狗：启动与喂狗
 *
 * 架构位置：APP BSP 层；与任务级 watchdog 双层防线
 * ================================================================ */
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
