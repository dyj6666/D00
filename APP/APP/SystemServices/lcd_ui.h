#ifndef LCD_UI_H
#define LCD_UI_H

#include <stdint.h>
#include "bsp_lcd.h"

/* ================================================================
 * LCD UI 框架（RTOS 极致应用层）
 *
 * 架构：
 *   - 独立渲染任务（LcdUI）：串行执行所有绘制命令（命令队列），
 *     事件总线/应用模块只发轻量请求，不阻塞、无并发写冲突；
 *   - 页面注册机制：任意模块注册页面（绘制/刷新函数），
 *     按键自动导航，新增显示界面只需注册一个页面；
 *   - 组件 API：文本/数值/填充/进度条/图形，一行调用；
 *   - 测试隔离：lcd test/bench/dir 等绘制在渲染任务内执行，
 *     与面板刷新完全互斥，杜绝画面参杂。
 * ================================================================ */

#define LCD_UI_MAX_PAGES   6

/* ---------- 页面描述 ---------- */
typedef struct {
    const char *name;              /* 页面名（页眉显示） */
    void (*draw)(void);            /* 整页绘制（切页时调用） */
    void (*refresh)(void);         /* 1s 周期刷新（可选，可传 NULL） */
    void (*touch)(uint8_t evt, uint16_t x, uint16_t y);  /* 可选：触摸回调
                                        （DOWN/MOVE 实时送达 + TAP；渲染任务
                                        上下文，框架仍负责光标与滑动切页） */
} lcd_ui_page_t;

/* ---------- 框架接口 ---------- */

/* 初始化：启动渲染任务（内部创建队列+任务），应用模块初始化时调用一次 */
void LcdUI_Init(void);

/* 注册页面（返回页号，-1 失败） */
int  LcdUI_AddPage(const lcd_ui_page_t *page);

/* 切换页面（发命令，渲染任务执行；越界钳位到末页） */
void LcdUI_ShowPage(uint8_t index);
int  LcdUI_NextPage(void);          /* 返回 0 成功 / -1 队列满丢弃 */
uint8_t LcdUI_GetPage(void);
uint16_t LcdUI_GetPendingCount(void); /* 待处理命令数（测试/监控用） */
uint32_t LcdUI_GetDroppedCount(void); /* 累计丢弃命令数 */

/* 进入/退出测试模式（暂停页面周期刷新，防止干扰测试绘制） */
void LcdUI_EnterTest(void);
void LcdUI_ExitTest(void);

/* 在渲染任务内执行一段绘制代码（测试画面等），与面板刷新完全互斥 */
void LcdUI_RunTest(void (*fn)(void));

/* ---------- 组件命令（异步发送，渲染任务串行执行） ---------- */

void LcdUI_Clear(uint16_t color);
void LcdUI_Fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                uint16_t color);
void LcdUI_Text(uint16_t x, uint16_t y, const char *s,
                uint16_t color, bsp_lcd_font_t font);
void LcdUI_Num(uint16_t x, uint16_t y, uint32_t value, uint8_t digits,
               uint16_t color, bsp_lcd_font_t font);
void LcdUI_Bar(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
               uint8_t pct, uint16_t color, uint16_t bg);
void LcdUI_Rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                uint16_t color);

#endif
