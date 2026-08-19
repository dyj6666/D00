/* ================================================================
 * gui_pages —— GUI 页面层：主页仪表盘 / 网络监控 / 系统监控
 *
 * 架构位置：APP 应用层；gui_app 提供任务与导航，本层负责
 *           页面构建、数据采集（1s 节拍）与控件刷新。
 * 数据来源：各服务/BSP 结构化接口（eth_app/icmp_svc/imu_svc/
 *           touch_svc/ota_agent/event_bus/data_link/usr_store/
 *           bsp_can/bsp_rtc/bsp_w25q128），不做直接寄存器访问。
 * ================================================================ */
#ifndef GUI_PAGES_H
#define GUI_PAGES_H

#include "lvgl.h"

/* 构建三个页面（屏幕对象）并挂到默认屏 */
void GuiPages_Init(void);

/* 返回主页屏幕对象（gui bench 基准用：可加载真实主页做 UI 场景测量） */
lv_obj_t *GuiPages_GetHome(void);

/* 页面切换（带方向动画）：由 gui_app 导航栏调用 */
void GuiPages_ShowHome(void);
void GuiPages_ShowNet(void);
void GuiPages_ShowSys(void);
void GuiPages_ShowCam(void);
void GuiPages_ShowGimbal(void);

/* 单按键翻页：HOME → NET → SYS → HOME 轮换（KEY0 短按导航，
 * 复用页面切换动画与连点保护；长按回主页走 GuiPages_ShowHome） */
void GuiPages_PageNext(void);

/* 250ms 快速节拍：三相轮转刷新（采集/曲线/文本分片，gui_task 调用，
 * 彻底错峰避免同帧爆发重绘；数据粒度 250ms 使数值更新更平滑） */
void GuiPages_RefreshFast(void);

#endif /* GUI_PAGES_H */
