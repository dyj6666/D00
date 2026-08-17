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
    /* 调试构建：APP 不初始化 IWDG，但 BOOT 跳转前启动的 IWDG 仍在运行
     * （IWDG 一旦启动不可关闭，BOOT 配置 128/4095 ≈ 16.4s 窗口）——
     * 必须继续喂，否则系统每 ~16s 被 BOOT 看门狗复位（重启循环）。 */
    IWDG->KR = 0xAAAA;
#else
    HAL_IWDG_Refresh(&hiwdg);
#endif
}
