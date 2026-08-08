#include "lcd_ui.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "cmsis_os2.h"

#include <string.h>

/* ================================================================
 * LCD UI 框架实现：独立渲染任务 + 命令队列 + 页面注册
 * ================================================================ */

#define UI_QUEUE_LEN    8
#define UI_TASK_STACK   2048   /* 字节（CMSIS-RTOS2）；v128 同负载在 1536B 任务内验证过 */
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
    for (;;) {
        ui_cmd_t cmd;
        /* 命令优先：队列有命令立即执行；空闲 1s 后刷新当前页 */
        if (xQueueReceive(ui_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
            ui_exec(&cmd);
            continue;
        }
        if (!ui_test_mode && ui_page_cur < ui_page_count &&
            ui_pages[ui_page_cur] != NULL &&
            ui_pages[ui_page_cur]->refresh != NULL) {
            ui_pages[ui_page_cur]->refresh();
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
