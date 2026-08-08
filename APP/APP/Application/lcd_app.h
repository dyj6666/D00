#ifndef LCD_APP_H
#define LCD_APP_H

/* 板载 LCD 系统信息面板：HOME/SYSTEM/BUS 三页，按键切换，1s 自动刷新。
 * 页面注册/导航/刷新由 LcdUI 渲染任务框架统一管理，本模块仅提供页面内容。 */
void LcdApp_Init(void);

#endif
