#include "lcd_app.h"
#include "LCD/lcd.h"
#include "event_bus.h"
#include "app_config.h"
#include "data_link.h"
#include "FreeRTOS.h"
#include "task.h"
#include "logger.h"

#include <string.h>
#include <stdio.h>

/* ================================================================
 * 板载 LCD 系统信息面板（官方 lcd 驱动，竖屏 240x320）
 *   - 三页：HOME(版本/CPU/堆) / SYSTEM(任务栈) / BUS(事件+HOSTLINK)
 *   - 按键短按切换页面（全屏重绘），1s 定时只刷新动态数值（无闪烁）
 * ================================================================ */

#define LCD_PAGE_COUNT   3
#define LCD_MAX_TASKS    16

static uint8_t lcd_page = 0;
static uint8_t lcd_ready = 0;
static uint32_t lcd_run_prev = 0, lcd_idle_prev = 0;
static uint8_t  lcd_cpu_pct = 0;

static uint16_t lcd_w(void) { return lcddev.width; }
static uint16_t lcd_h(void) { return lcddev.height; }

static void lcd_page_home_values(void);
static void lcd_page_bus_values(void);

/* ---------- 文本助手（官方无背景色参数，用全局背景色） ---------- */
static void lcd_text(uint16_t x, uint16_t y, const char *s,
                     uint16_t color, uint8_t size)
{
    lcd_show_string(x, y, lcd_w(), lcd_h(), size, (char *)s, color);
}

/* ---------- 页眉 / 页脚 ---------- */
static void lcd_header(const char *title)
{
    uint16_t w = lcd_w();
    lcd_fill(0, 0, (uint16_t)(w - 1), 23, BLUE);
    lcd_text(4, 4, title, WHITE, 16);
    lcd_text((uint16_t)(w - 36), 4, "D00", YELLOW, 16);
}

static void lcd_footer(uint8_t page)
{
    uint16_t w = lcd_w();
    uint16_t h = lcd_h();
    char bar[24];
    snprintf(bar, sizeof(bar), "KEY: PAGE %u/%u", page + 1, LCD_PAGE_COUNT);
    lcd_fill(0, (uint16_t)(h - 12), (uint16_t)(w - 1), (uint16_t)(h - 1),
             GRAY);
    lcd_text(4, (uint16_t)(h - 11), bar, WHITE, 8);
}

/* ---------- CPU 差分 ---------- */
static void lcd_update_cpu(void)
{
    TaskStatus_t *arr = pvPortMalloc(LCD_MAX_TASKS * sizeof(TaskStatus_t));
    if (arr == NULL) return;
    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(arr, LCD_MAX_TASKS, &total);
    uint32_t idle_run = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        if (strcmp(arr[i].pcTaskName, "IDLE") == 0) {
            idle_run = arr[i].ulRunTimeCounter;
            break;
        }
    }
    vPortFree(arr);

    uint32_t d_run = total - lcd_run_prev;
    uint32_t d_idle = idle_run - lcd_idle_prev;
    if (d_run > 0 && d_idle <= d_run) {
        lcd_cpu_pct = (uint8_t)(100u - (uint32_t)((uint64_t)d_idle * 100u / d_run));
    }
    lcd_run_prev = total;
    lcd_idle_prev = idle_run;
}

/* ---------- HOME ---------- */
static void lcd_page_home_values(void)
{
    char buf[24];
    uint32_t uptime = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    snprintf(buf, sizeof(buf), "%7lus", (unsigned long)(uptime / 1000u));
    lcd_text(100, 116, buf, WHITE, 16);
    snprintf(buf, sizeof(buf), "%3u%%", lcd_cpu_pct);
    lcd_text(100, 142, buf, GREEN, 16);
    snprintf(buf, sizeof(buf), "%7luB", (unsigned long)xPortGetFreeHeapSize());
    lcd_text(100, 168, buf, GREEN, 16);
    snprintf(buf, sizeof(buf), "%3lu", (unsigned long)uxTaskGetNumberOfTasks());
    lcd_text(100, 194, buf, WHITE, 16);
}

static void lcd_page_home_frame(void)
{
    uint32_t ver = *(volatile uint32_t *)OTA_APP_VERSION_ADDR;
    char buf[24];

    lcd_text(8, 36, "D00 Platform", YELLOW, 16);
    lcd_text(8, 58, "STM32F407 Industrial", CYAN, 12);
    lcd_text(8, 94, "Firmware", GRAY, 12);
    lcd_text(8, 120, "Uptime", GRAY, 12);
    lcd_text(8, 146, "CPU", GRAY, 12);
    lcd_text(8, 172, "Heap", GRAY, 12);
    lcd_text(8, 198, "Tasks", GRAY, 12);

    snprintf(buf, sizeof(buf), "v%lu", (unsigned long)ver);
    lcd_text(100, 90, buf, WHITE, 16);
    lcd_page_home_values();
}

/* ---------- SYSTEM ---------- */
static void lcd_page_system_draw(void)
{
    TaskStatus_t *arr = pvPortMalloc(LCD_MAX_TASKS * sizeof(TaskStatus_t));
    if (arr == NULL) return;
    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(arr, LCD_MAX_TASKS, &total);
    if (n > 12) n = 12;

    lcd_text(4, 28, "TASK", YELLOW, 8);
    lcd_text(130, 28, "STACK", YELLOW, 8);
    lcd_text(210, 28, "PRIO", YELLOW, 8);

    for (UBaseType_t i = 0; i < n; i++) {
        uint16_t y = (uint16_t)(40 + i * 20);
        lcd_fill(4, y, (uint16_t)(lcd_w() - 5), (uint16_t)(y + 15), BLACK);
        lcd_text(4, y, arr[i].pcTaskName, WHITE, 8);
        lcd_show_num(130, y, arr[i].usStackHighWaterMark, 4, 8, GREEN);
        lcd_show_num(210, y, arr[i].uxCurrentPriority, 2, 8, CYAN);
    }
    vPortFree(arr);
}

/* ---------- BUS ---------- */
static void lcd_page_bus_values(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%10lu", (unsigned long)EventBus_GetLostCount());
    lcd_text(100, 52, buf, WHITE, 16);
    snprintf(buf, sizeof(buf), "%5lu/%lu", (unsigned long)EventBus_GetQueueCount(),
             (unsigned long)EventBus_GetPoolFreeCount());
    lcd_text(100, 80, buf, WHITE, 16);
    snprintf(buf, sizeof(buf), "%10lu", (unsigned long)DataLink_GetTxLostCount());
    lcd_text(100, 142, buf, WHITE, 16);
    snprintf(buf, sizeof(buf), "%10lu", (unsigned long)DataLink_GetTxErrorCount());
    lcd_text(100, 170, buf, WHITE, 16);
    snprintf(buf, sizeof(buf), "%10lu", (unsigned long)DataLink_GetCmdLostCount());
    lcd_text(100, 198, buf, WHITE, 16);
}

static void lcd_page_bus_frame(void)
{
    lcd_text(4, 28, "EVENT BUS", YELLOW, 16);
    lcd_text(4, 56, "Lost", GRAY, 12);
    lcd_text(4, 84, "Queue/Pool", GRAY, 12);
    lcd_text(4, 118, "HOSTLINK", YELLOW, 16);
    lcd_text(4, 146, "TX lost", GRAY, 12);
    lcd_text(4, 174, "TX err", GRAY, 12);
    lcd_text(4, 202, "Cmd lost", GRAY, 12);
    lcd_page_bus_values();
}

/* ---------- 页面切换 ---------- */
static void lcd_draw_page(void)
{
    static const char *titles[LCD_PAGE_COUNT] = {
        "HOME", "SYSTEM", "BUS"
    };
    lcd_fill(0, 24, (uint16_t)(lcd_w() - 1), (uint16_t)(lcd_h() - 13), BLACK);
    lcd_header(titles[lcd_page]);
    if (lcd_page == 0) lcd_page_home_frame();
    else if (lcd_page == 1) lcd_page_system_draw();
    else lcd_page_bus_frame();
    lcd_footer(lcd_page);
}

static void lcd_refresh_values(void)
{
    if (lcd_page == 0) lcd_page_home_values();
    else if (lcd_page == 1) lcd_page_system_draw();
    else lcd_page_bus_values();
}

/* ---------- 事件 ---------- */
static void lcd_on_tick(const message_t *msg)
{
    (void)msg;
    if (!lcd_ready) return;
    lcd_update_cpu();
    lcd_refresh_values();
}

static void lcd_on_key(const message_t *msg)
{
    if (!lcd_ready || msg == NULL) return;
    if (msg->hdr.type == MSG_KEY_SHORT) {
        lcd_page = (uint8_t)((lcd_page + 1) % LCD_PAGE_COUNT);
        lcd_draw_page();
    }
}

/* ---------- 初始化 ---------- */
void LcdApp_Init(void)
{
    g_back_color = BLACK;      /* 官方驱动全局背景色 */
    lcd_init();
    lcd_display_dir(0);        /* 竖屏（官方已验证） */
    LCD_BL(1);
    lcd_clear(BLACK);
    LOG_Printf("[APP] LCD  : id=0x%04X, %ux%u\r\n",
               lcddev.id, lcddev.width, lcddev.height);

    lcd_ready = 1;
    lcd_update_cpu();
    lcd_draw_page();

    EventBus_Subscribe(MSG_TICK_1S, lcd_on_tick);
    EventBus_Subscribe(MSG_KEY_SHORT, lcd_on_key);
}
