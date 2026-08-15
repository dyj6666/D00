/* ================================================================
 * lv_port —— LVGL 平台移植层总入口实现
 * ================================================================ */
#include "lv_port.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"

void LvPort_Init(void)
{
    LvPort_DispInit();     /* 显示：FSMC LCD flush 注册 */
    LvPort_IndevInit();    /* 输入：触摸轮询注册 */
}
