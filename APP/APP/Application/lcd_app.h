#ifndef LCD_APP_H
#define LCD_APP_H

/* 板载 LCD 系统信息面板：主页/系统/总线三页，按键切换，1s 自动刷新 */
void LcdApp_Init(void);

/* 进入/退出测试模式：测试期间暂停 1s 面板刷新（避免实时数据覆盖测试画面），
 * 退出时自动重绘面板恢复干净显示。 */
void LcdApp_EnterTest(void);
void LcdApp_ExitTest(void);

#endif
