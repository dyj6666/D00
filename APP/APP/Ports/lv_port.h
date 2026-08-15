/* ================================================================
 * lv_port —— LVGL 平台移植层总入口
 *
 * 架构位置：APP Ports 层；向下只依赖 BSP（bsp_lcd / touch_svc），
 *           向上对 Application 暴露单一初始化入口。LVGL 库本体位于
 *           Middlewares/Third_Party/lvgl，业务层禁止直接触碰端口细节。
 * ================================================================ */
#ifndef LV_PORT_H
#define LV_PORT_H

#include <stdint.h>

/* 初始化全部 LVGL 端口（显示 + 输入；tick 为查询式无初始化）。
 * 须在 lv_init() 之后、创建界面之前调用一次。 */
void LvPort_Init(void);

#endif /* LV_PORT_H */
