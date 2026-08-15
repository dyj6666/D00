/* ================================================================
 * gui_app —— GUI 应用层（LVGL 核心链接）
 *
 * 架构位置：APP 应用层；向下经 Ports 层驱动 LVGL，取代旧自绘
 *           lcd_app/lcd_ui 页面层成为系统唯一显示出口。模块注册
 *           入口（module.c）调 GuiApp_Init。
 * ================================================================ */
#ifndef GUI_APP_H
#define GUI_APP_H

/* 初始化 GUI：LCD 硬件 + LVGL 核心 + 端口 + 界面 + 渲染任务 */
void GuiApp_Init(void);

#endif /* GUI_APP_H */
