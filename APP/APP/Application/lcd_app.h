/* ================================================================
 * lcd_app —— LCD 应用：初始化与页面切换
 *
 * 架构位置：APP 应用层；lcd_ui 页面调度
 * ================================================================ */
#ifndef LCD_APP_H
#define LCD_APP_H

#include <stdint.h>

/* 板载 LCD 系统信息面板：HOME/SYSTEM/BUS 三页，按键切换，1s 自动刷新。
 * 页面注册/导航/刷新由 LcdUI 渲染任务框架统一管理，本模块仅提供页面内容。 */
void LcdApp_Init(void);

/** @brief 当前 LCD 显示的 CPU 占用率（0-100%，由 1s 差分采样维护） */
uint8_t LcdApp_GetCpuPct(void);

#endif
