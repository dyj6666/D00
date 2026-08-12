/* ================================================================
 * lcd_ui —— LCD 用户界面：状态页/菜单/ETH 全状态绘制
 *
 * 架构位置：APP 服务层；独立 UI 任务周期刷新
 * ================================================================ */
#include "lcd_ui.h"
#include "touch_svc.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "cmsis_os2.h"

#include <string.h>

/* ================================================================
 * LCD UI 框架实现：独立渲染任务 + 命令队列 + 页面注册
 * ================================================================ */

#define UI_QUEUE_LEN    8
#define UI_TASK_STACK   2560   /* 字节（CMSIS-RTOS2）；含 3D 渲染/格式化栈帧余量。
                                * 注意：Keil HW 实测 724B 不适用于 GCC——
                                * GCC 下大 ui_cmd_t(50B) + snprintf + 浮点路径
                                * 栈深明显更大，1536B 会溢出损坏页面命令
                                * （实测画面重叠/花屏/错误页背景）。 */
#define UI_TEXT_MAX     32

typedef enum {
    UI_CMD_CLEAR,
    UI_CMD_FILL,
    UI_CMD_TEXT,
    UI_CMD_NUM,
    UI_CMD_BAR,
    UI_CMD_RECT,
    UI_CMD_SHOW_PAGE,
    UI_CMD_NEXT_PAGE,
    UI_CMD_ENTER_TEST,
    UI_CMD_EXIT_TEST,
    UI_CMD_RUN_TEST,
} ui_cmd_type_t;

typedef struct {
    uint8_t  type;
    uint16_t x, y, x1, y1, w, h;
    uint16_t color, color2;
    uint32_t value;
    uint8_t  digits, font;
    union {
        char text[UI_TEXT_MAX];   /* UI_CMD_TEXT */
        void (*fn)(void);         /* UI_CMD_RUN_TEST */
    } u;
} ui_cmd_t;

static QueueHandle_t ui_queue = NULL;
static const lcd_ui_page_t *ui_pages[LCD_UI_MAX_PAGES];
static uint8_t ui_page_count = 0;
static volatile uint8_t ui_page_cur = 0;
static volatile uint8_t ui_test_mode = 0;
static volatile uint32_t ui_dropped = 0;

/* ---------- 触摸光标状态（渲染任务私有） ---------- */
#define UI_CURSOR_SIZE   8   /* 8x8 光标 */
#define UI_CURSOR_HALF   3   /* 中心偏移 */
#define UI_MOVE_DEADBAND 2   /* 位移死区（px），吸收亚像素抖动 */
#define UI_SWIPE_DIST    40  /* 滑动判定阈值（px） */
static uint32_t s_touch_gen = 0;
static uint8_t  s_touch_active = 0;
static uint16_t s_cursor_x = 0, s_cursor_y = 0;
static uint16_t s_cursor_save[UI_CURSOR_SIZE * UI_CURSOR_SIZE];  /* 原色备份 */

/* 光标左上角（边界钳位，保持 8x8） */
static void ui_cursor_rect(uint16_t x, uint16_t y, uint16_t *x0, uint16_t *y0)
{
    int16_t a = (int16_t)x - (int16_t)UI_CURSOR_HALF;
    int16_t b = (int16_t)y - (int16_t)UI_CURSOR_HALF;
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (a > (int16_t)(BSP_LCD_GetWidth() - UI_CURSOR_SIZE))
        a = (int16_t)(BSP_LCD_GetWidth() - UI_CURSOR_SIZE);
    if (b > (int16_t)(BSP_LCD_GetHeight() - UI_CURSOR_SIZE))
        b = (int16_t)(BSP_LCD_GetHeight() - UI_CURSOR_SIZE);
    *x0 = (uint16_t)a;
    *y0 = (uint16_t)b;
}

/* 擦除：把之前保存的原色逐像素写回（无黑洞、无拖影） */
static void ui_cursor_erase(void)
{
    uint16_t x0, y0;
    ui_cursor_rect(s_cursor_x, s_cursor_y, &x0, &y0);
    BSP_LCD_WritePixels(x0, y0, UI_CURSOR_SIZE, UI_CURSOR_SIZE,
                        s_cursor_save);
}

/* 绘制：先读回原色备份，再画白色光标 */
static void ui_cursor_draw(uint16_t x, uint16_t y)
{
    uint16_t x0, y0;
    ui_cursor_rect(x, y, &x0, &y0);
    BSP_LCD_ReadPixels(x0, y0, UI_CURSOR_SIZE, UI_CURSOR_SIZE,
                       s_cursor_save);
    BSP_LCD_Fill(x0, y0, (uint16_t)(x0 + UI_CURSOR_SIZE - 1),
                 (uint16_t)(y0 + UI_CURSOR_SIZE - 1), BSP_LCD_COLOR_WHITE);
}

/* ---------- 触摸事件处理（渲染任务上下文，串行绘制） ---------- */
static void ui_handle_touch(const touch_svc_state_t *ts)
{
    switch (ts->state) {
    case TOUCH_EVT_DOWN:
    case TOUCH_EVT_MOVE:
        if (!ui_test_mode) {   /* 测试/校准画面保持纯净，不画光标 */
            if (!s_touch_active) {
                s_touch_active = 1;
                s_cursor_x = ts->x;
                s_cursor_y = ts->y;
                ui_cursor_draw(ts->x, ts->y);
            } else {
                /* 死区：位移 <2px 不重绘（吸收抖动） */
                uint16_t adx = (ts->x > s_cursor_x)
                                   ? (uint16_t)(ts->x - s_cursor_x)
                                   : (uint16_t)(s_cursor_x - ts->x);
                uint16_t ady = (ts->y > s_cursor_y)
                                   ? (uint16_t)(ts->y - s_cursor_y)
                                   : (uint16_t)(s_cursor_y - ts->y);
                if (adx >= UI_MOVE_DEADBAND || ady >= UI_MOVE_DEADBAND) {
                    ui_cursor_erase();
                    s_cursor_x = ts->x;
                    s_cursor_y = ts->y;
                    ui_cursor_draw(ts->x, ts->y);
                }
            }
        }
        /* 页面实时触摸反馈（可选） */
        if (ui_page_cur < ui_page_count && ui_pages[ui_page_cur] != NULL &&
            ui_pages[ui_page_cur]->touch != NULL) {
            ui_pages[ui_page_cur]->touch(ts->state, ts->x, ts->y);
        }
        break;

    case TOUCH_EVT_UP:
        if (s_touch_active) ui_cursor_erase();
        s_touch_active = 0;
        /* 手势：触摸全程最大横向位移 >40px 且占优 → 切页
         * （接触抖动/中途瞬断不影响判定）；否则 tap → 页面回调 */
        {
            if (ts->max_dx > UI_SWIPE_DIST && ts->max_dx > ts->max_dy) {
                LcdUI_NextPage();   /* 滑动切页 */
            } else if (ts->max_dy > UI_SWIPE_DIST && ts->max_dy > ts->max_dx) {
                /* 纵向滑动 → 页面滚动回调（SYSTEM 任务列表上下查看） */
                uint8_t evt = (ts->up_y < ts->down_y) ? TOUCH_EVT_SWIPE_UP
                                                      : TOUCH_EVT_SWIPE_DOWN;
                if (ui_page_cur < ui_page_count &&
                    ui_pages[ui_page_cur] != NULL &&
                    ui_pages[ui_page_cur]->touch != NULL) {
                    ui_pages[ui_page_cur]->touch(evt, ts->up_x, ts->up_y);
                }
            } else if (ui_page_cur < ui_page_count &&
                       ui_pages[ui_page_cur] != NULL &&
                       ui_pages[ui_page_cur]->touch != NULL) {
                ui_pages[ui_page_cur]->touch(TOUCH_EVT_TAP, ts->up_x, ts->up_y);
            }
        }
        break;

    default:
        break;
    }
}

/* ---------- 当前页绘制 ---------- */
static void ui_draw_page(void)
{
    if (ui_page_cur < ui_page_count && ui_pages[ui_page_cur] != NULL &&
        ui_pages[ui_page_cur]->draw != NULL) {
        ui_pages[ui_page_cur]->draw();
    }
}

/* ---------- 命令执行（渲染任务上下文，串行） ---------- */
static void ui_exec(const ui_cmd_t *cmd)
{
    switch (cmd->type) {
    case UI_CMD_CLEAR:
        BSP_LCD_Clear(cmd->color);
        break;
    case UI_CMD_FILL:
        BSP_LCD_Fill(cmd->x, cmd->y, cmd->x1, cmd->y1, cmd->color);
        break;
    case UI_CMD_TEXT:
        BSP_LCD_ShowString(cmd->x, cmd->y, cmd->u.text, cmd->color,
                           (bsp_lcd_font_t)cmd->font);
        break;
    case UI_CMD_NUM:
        BSP_LCD_ShowNum(cmd->x, cmd->y, cmd->value, cmd->digits,
                        cmd->color, (bsp_lcd_font_t)cmd->font);
        break;
    case UI_CMD_BAR: {
        /* 进度条：bg 底色 + value(0-100)% 前景 */
        uint8_t pct = (cmd->value > 100u) ? 100u : (uint8_t)cmd->value;
        uint16_t fw = (uint16_t)((uint32_t)cmd->w * pct / 100u);
        BSP_LCD_Fill(cmd->x, cmd->y,
                     (uint16_t)(cmd->x + cmd->w - 1),
                     (uint16_t)(cmd->y + cmd->h - 1), cmd->color2);
        if (fw > 0) {
            BSP_LCD_Fill(cmd->x, cmd->y,
                         (uint16_t)(cmd->x + fw - 1),
                         (uint16_t)(cmd->y + cmd->h - 1), cmd->color);
        }
        break;
    }
    case UI_CMD_RECT:
        BSP_LCD_DrawRect(cmd->x, cmd->y, cmd->x1, cmd->y1, cmd->color);
        break;
    case UI_CMD_SHOW_PAGE:
        if (cmd->value < ui_page_count) {
            ui_page_cur = (uint8_t)cmd->value;
        } else if (ui_page_count > 0) {
            ui_page_cur = (uint8_t)(ui_page_count - 1);
        }
        ui_draw_page();
        vTaskDelay(pdMS_TO_TICKS(40));        /* 等待面板刷新两帧，防撕裂 */
        break;
    case UI_CMD_NEXT_PAGE:
        if (ui_page_count > 0) {
            ui_page_cur = (uint8_t)((ui_page_cur + 1) % ui_page_count);
        }
        ui_draw_page();
        vTaskDelay(pdMS_TO_TICKS(40));
        break;
    case UI_CMD_ENTER_TEST:
        ui_test_mode = 1;
        break;
    case UI_CMD_EXIT_TEST:
        ui_test_mode = 0;
        ui_draw_page();          /* 恢复面板 */
        vTaskDelay(pdMS_TO_TICKS(40));
        break;
    case UI_CMD_RUN_TEST:
        if (cmd->u.fn != NULL) {
            cmd->u.fn();
        }
        break;
    default:
        break;
    }
}

/* ---------- 渲染任务 ---------- */
static void lcd_ui_task(void *arg)
{
    (void)arg;
    uint32_t refresh_last = 0;
    for (;;) {
        ui_cmd_t cmd;
        /* 固定 8ms 节拍：命令优先，触摸始终低延迟轮询，
         * 周期刷新由 1s 时间闸门控制（非触摸/非测试时） */
        if (xQueueReceive(ui_queue, &cmd, pdMS_TO_TICKS(8)) == pdTRUE) {
            ui_exec(&cmd);
            continue;
        }
        /* 触摸事件轮询 */
        {
            const touch_svc_state_t *ts = TouchSvc_GetState();
            if (ts->gen != s_touch_gen) {
                s_touch_gen = ts->gen;
                ui_handle_touch(ts);
                continue;
            }
        }
        /* 空闲 1s 周期刷新（触摸期间暂停，保证光标丝滑） */
        if (!s_touch_active && !ui_test_mode && ui_page_cur < ui_page_count &&
            ui_pages[ui_page_cur] != NULL &&
            ui_pages[ui_page_cur]->refresh != NULL) {
            uint32_t now = (uint32_t)xTaskGetTickCount();
            uint16_t period = ui_pages[ui_page_cur]->refresh_ms;
            if (period == 0) period = 1000;
            if (now - refresh_last >= pdMS_TO_TICKS(period)) {
                refresh_last = now;
                ui_pages[ui_page_cur]->refresh();
            }
        }
    }
}

/* ---------- 发送（非阻塞，队列满丢弃） ---------- */
static int ui_send(const ui_cmd_t *cmd)
{
    if (ui_queue == NULL) return -1;
    if (xQueueSend(ui_queue, cmd, 0) == pdTRUE) return 0;
    ui_dropped++;
    return -1;
}

/* ---------- 框架接口 ---------- */
void LcdUI_Init(void)
{
    if (ui_queue != NULL) return;
    ui_queue = xQueueCreate(UI_QUEUE_LEN, sizeof(ui_cmd_t));
    configASSERT(ui_queue);

    osThreadAttr_t attr = {
        .name = "LcdUI",
        .stack_size = UI_TASK_STACK,
        .priority = osPriorityNormal,
    };
    osThreadNew(lcd_ui_task, NULL, &attr);
}

int LcdUI_AddPage(const lcd_ui_page_t *page)
{
    if (page == NULL || ui_page_count >= LCD_UI_MAX_PAGES) return -1;
    ui_pages[ui_page_count++] = page;
    return (int)(ui_page_count - 1);
}

void LcdUI_ShowPage(uint8_t index)
{
    ui_cmd_t c = {0};
    c.type = UI_CMD_SHOW_PAGE;
    c.value = index;
    ui_send(&c);
}

int LcdUI_NextPage(void)
{
    ui_cmd_t c = {0};
    c.type = UI_CMD_NEXT_PAGE;
    return ui_send(&c);
}

uint8_t LcdUI_GetPage(void)
{
    return ui_page_cur;
}

uint16_t LcdUI_GetPendingCount(void)
{
    if (ui_queue == NULL) return 0;
    return (uint16_t)uxQueueMessagesWaiting(ui_queue);
}

uint32_t LcdUI_GetDroppedCount(void)
{
    return ui_dropped;
}

void LcdUI_EnterTest(void)
{
    ui_cmd_t c = {0};
    c.type = UI_CMD_ENTER_TEST;
    ui_send(&c);
}

void LcdUI_ExitTest(void)
{
    ui_cmd_t c = {0};
    c.type = UI_CMD_EXIT_TEST;
    ui_send(&c);
}

void LcdUI_RunTest(void (*fn)(void))
{
    if (fn == NULL) return;
    ui_cmd_t c = {0};
    c.type = UI_CMD_RUN_TEST;
    c.u.fn = fn;
    ui_send(&c);
}

/* ---------- 组件命令 ---------- */
void LcdUI_Clear(uint16_t color)
{
    ui_cmd_t c = {0};
    c.type = UI_CMD_CLEAR;
    c.color = color;
    ui_send(&c);
}

void LcdUI_Fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                uint16_t color)
{
    ui_cmd_t c = {0};
    c.type = UI_CMD_FILL;
    c.x = x0; c.y = y0; c.x1 = x1; c.y1 = y1; c.color = color;
    ui_send(&c);
}

void LcdUI_Text(uint16_t x, uint16_t y, const char *s,
                uint16_t color, bsp_lcd_font_t font)
{
    if (s == NULL) return;
    ui_cmd_t c = {0};
    c.type = UI_CMD_TEXT;
    c.x = x; c.y = y; c.color = color; c.font = (uint8_t)font;
    strncpy(c.u.text, s, UI_TEXT_MAX - 1);
    c.u.text[UI_TEXT_MAX - 1] = '\0';
    ui_send(&c);
}

void LcdUI_Num(uint16_t x, uint16_t y, uint32_t value, uint8_t digits,
               uint16_t color, bsp_lcd_font_t font)
{
    ui_cmd_t c = {0};
    c.type = UI_CMD_NUM;
    c.x = x; c.y = y; c.value = value;
    c.digits = digits; c.color = color; c.font = (uint8_t)font;
    ui_send(&c);
}

void LcdUI_Bar(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
               uint8_t pct, uint16_t color, uint16_t bg)
{
    ui_cmd_t c = {0};
    c.type = UI_CMD_BAR;
    c.x = x; c.y = y; c.w = w; c.h = h;
    c.value = pct; c.color = color; c.color2 = bg;
    ui_send(&c);
}

void LcdUI_Rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                uint16_t color)
{
    ui_cmd_t c = {0};
    c.type = UI_CMD_RECT;
    c.x = x0; c.y = y0; c.x1 = x1; c.y1 = y1; c.color = color;
    ui_send(&c);
}
