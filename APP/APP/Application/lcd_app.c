#include "lcd_app.h"
#include "bsp_lcd.h"
#include "lcd_ui.h"
#include "event_bus.h"
#include "app_config.h"
#include "data_link.h"
#include "FreeRTOS.h"
#include "task.h"
#include "logger.h"

#include <string.h>
#include <stdio.h>

/* ================================================================
 * 板载 LCD 系统信息面板（基于 LcdUI 渲染任务框架）
 *   - HOME/SYSTEM/BUS 三页注册，按键切换，1s 周期刷新
 *   - 布局规范：标签左列 x=8（FONT12），数值右对齐 x=150（FONT16），
 *     行距 26px，数值与标签垂直居中对齐（FONT16 上移 2px）
 *   - SYSTEM 1s 刷新只重画任务列表（行内先清后画），不整页重绘 → 无频闪
 * ================================================================ */

#define LCD_MAX_TASKS    16
#define LCD_VAL_RIGHT    150   /* 数值右对齐边线（FONT16，8px/字符） */

static uint32_t lcd_run_prev = 0, lcd_idle_prev = 0;
static uint8_t  lcd_cpu_pct = 0;
static uint32_t lcd_cpu_last_ms = 0;
/* 任务状态缓冲：静态分配，避免每秒 pvPortMalloc 的堆抖动与碎片化。
 * 仅在 LcdUI 渲染任务内使用（各页 refresh 串行执行），无并发风险。 */
static TaskStatus_t s_lcd_tasks[LCD_MAX_TASKS];

/* ---------- CPU 差分（固定 1s 采样窗口） ----------
 * 设计：
 *   - 三页 refresh 均调用 → 采样节奏恒为 1s，与切页时机无关；
 *   - 短间隔（<900ms，快速切页）沿用上次值，避免绘制突发污染读数；
 *   - 超长间隔（>5s）仅重建基线，防止 DWT(168MHz) 25.6s 回绕算出错误值。 */
static void lcd_update_cpu(void)
{
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t dt = now_ms - lcd_cpu_last_ms;
    if (dt < 900u) return;   /* 短间隔：不采样，沿用上次值 */

    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(s_lcd_tasks, LCD_MAX_TASKS, &total);
    uint32_t idle_run = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        if (strcmp(s_lcd_tasks[i].pcTaskName, "IDLE") == 0) {
            idle_run = s_lcd_tasks[i].ulRunTimeCounter;
            break;
        }
    }

    uint32_t d_run = total - lcd_run_prev;
    uint32_t d_idle = idle_run - lcd_idle_prev;
    if (dt <= 5000u && lcd_run_prev != 0 && d_run > 0 && d_idle <= d_run) {
        lcd_cpu_pct = (uint8_t)(100u - (uint32_t)((uint64_t)d_idle * 100u / d_run));
    }
    lcd_run_prev = total;
    lcd_idle_prev = idle_run;
    lcd_cpu_last_ms = now_ms;
}

/* ---------- 页眉 / 页脚 / 内容区 ---------- */
static void lcd_page_header(const char *title)
{
    uint16_t w = BSP_LCD_GetWidth();
    BSP_LCD_Fill(0, 0, (uint16_t)(w - 1), 23, BSP_LCD_COLOR_BLUE);
    BSP_LCD_ShowString(4, 4, title, BSP_LCD_COLOR_WHITE, BSP_LCD_FONT_16);
    BSP_LCD_ShowString((uint16_t)(w - 36), 4, "D00", BSP_LCD_COLOR_YELLOW,
                       BSP_LCD_FONT_16);
}

static void lcd_page_footer(const char *hint)
{
    uint16_t w = BSP_LCD_GetWidth();
    uint16_t h = BSP_LCD_GetHeight();
    BSP_LCD_Fill(0, (uint16_t)(h - 14), (uint16_t)(w - 1),
                 (uint16_t)(h - 1), BSP_LCD_COLOR_GRAY);
    BSP_LCD_ShowString(4, (uint16_t)(h - 13), hint, BSP_LCD_COLOR_WHITE,
                       BSP_LCD_FONT_12);
}

static void lcd_page_clear_content(void)
{
    BSP_LCD_Fill(0, 24, (uint16_t)(BSP_LCD_GetWidth() - 1),
                 (uint16_t)(BSP_LCD_GetHeight() - 15),
                 BSP_LCD_COLOR_BLACK);
}

/* ---------- 数值（FONT16，右对齐至 x=LCD_VAL_RIGHT） ---------- */
static void lcd_val(uint16_t y, const char *s, uint16_t color)
{
    uint16_t n = (uint16_t)strlen(s);
    BSP_LCD_ShowString((uint16_t)(LCD_VAL_RIGHT - n * 8u), y, s, color,
                       BSP_LCD_FONT_16);
}

/* ================= HOME 页 ================= */
static void lcd_home_refresh(void)
{
    char buf[24];
    uint32_t uptime = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    lcd_update_cpu();

    snprintf(buf, sizeof(buf), "v%lu", (unsigned long)
             *(volatile uint32_t *)OTA_APP_VERSION_ADDR);
    lcd_val(92, buf, BSP_LCD_COLOR_WHITE);
    snprintf(buf, sizeof(buf), "%lus", (unsigned long)(uptime / 1000u));
    lcd_val(118, buf, BSP_LCD_COLOR_WHITE);
    snprintf(buf, sizeof(buf), "%u%%", lcd_cpu_pct);
    lcd_val(144, buf, BSP_LCD_COLOR_GREEN);
    snprintf(buf, sizeof(buf), "%luB", (unsigned long)xPortGetFreeHeapSize());
    lcd_val(170, buf, BSP_LCD_COLOR_GREEN);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)uxTaskGetNumberOfTasks());
    lcd_val(196, buf, BSP_LCD_COLOR_WHITE);
}

static void lcd_home_draw(void)
{
    uint16_t w = BSP_LCD_GetWidth();
    lcd_page_clear_content();
    lcd_page_header("HOME");
    BSP_LCD_ShowString(8, 36, "D00 Platform", BSP_LCD_COLOR_YELLOW,
                       BSP_LCD_FONT_16);
    BSP_LCD_ShowString(8, 58, "STM32F407 Industrial", BSP_LCD_COLOR_CYAN,
                       BSP_LCD_FONT_12);
    /* 标题与信息区分隔线 */
    BSP_LCD_Fill(8, 80, (uint16_t)(w - 9), 81, BSP_LCD_COLOR_GRAY);

    BSP_LCD_ShowString(8, 94, "Firmware", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 120, "Uptime", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 146, "CPU", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 172, "Heap", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 198, "Tasks", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);

    lcd_home_refresh();
    lcd_page_footer("KEY: NEXT PAGE");
}

/* ================= SYSTEM 页 ================= */
static void lcd_system_list(void)
{
    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(s_lcd_tasks, LCD_MAX_TASKS, &total);
    if (n > 12) n = 12;
    /* 固定排序：优先级降序 + 名称，避免链表遍历顺序不稳定导致行跳动 */
    for (UBaseType_t i = 1; i < n; i++) {
        TaskStatus_t key = s_lcd_tasks[i];
        int j = (int)i - 1;
        while (j >= 0 &&
               (s_lcd_tasks[j].uxCurrentPriority < key.uxCurrentPriority ||
                (s_lcd_tasks[j].uxCurrentPriority == key.uxCurrentPriority &&
                 strcmp(s_lcd_tasks[j].pcTaskName, key.pcTaskName) > 0))) {
            s_lcd_tasks[j + 1] = s_lcd_tasks[j];
            j--;
        }
        s_lcd_tasks[j + 1] = key;
    }

    BSP_LCD_ShowString(4, 28, "TASK", BSP_LCD_COLOR_YELLOW, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(130, 28, "STACK", BSP_LCD_COLOR_YELLOW, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(210, 28, "PRIO", BSP_LCD_COLOR_YELLOW, BSP_LCD_FONT_12);

    for (UBaseType_t i = 0; i < n; i++) {
        uint16_t y = (uint16_t)(40 + i * 20);
        /* 行内先清后画：只刷新本行，不整页重绘（无频闪） */
        BSP_LCD_Fill(4, y, (uint16_t)(BSP_LCD_GetWidth() - 5),
                     (uint16_t)(y + 15), BSP_LCD_COLOR_BLACK);
        BSP_LCD_ShowString(4, y, s_lcd_tasks[i].pcTaskName, BSP_LCD_COLOR_WHITE,
                           BSP_LCD_FONT_12);
        BSP_LCD_ShowNum(130, y, s_lcd_tasks[i].usStackHighWaterMark, 4,
                        BSP_LCD_COLOR_GREEN, BSP_LCD_FONT_12);
        BSP_LCD_ShowNum(210, y, s_lcd_tasks[i].uxCurrentPriority, 2,
                        BSP_LCD_COLOR_CYAN, BSP_LCD_FONT_12);
    }
}

static void lcd_system_draw(void)
{
    lcd_page_clear_content();
    lcd_page_header("SYSTEM");
    lcd_system_list();
    lcd_page_footer("KEY: NEXT PAGE");
}

static void lcd_system_refresh(void)
{
    lcd_update_cpu();      /* 保持 1s 采样节奏（切页时读数稳定） */
    lcd_system_list();   /* 只刷新任务列表，整页帧保持不动 → 无频闪 */
}

/* ================= BUS 页 ================= */
static void lcd_bus_refresh(void)
{
    char buf[24];
    lcd_update_cpu();      /* 保持 1s 采样节奏 */
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)EventBus_GetLostCount());
    lcd_val(60, buf, BSP_LCD_COLOR_WHITE);
    snprintf(buf, sizeof(buf), "%lu/%lu",
             (unsigned long)EventBus_GetQueueCount(),
             (unsigned long)EventBus_GetPoolFreeCount());
    lcd_val(86, buf, BSP_LCD_COLOR_WHITE);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)DataLink_GetTxLostCount());
    lcd_val(156, buf, BSP_LCD_COLOR_WHITE);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)DataLink_GetTxErrorCount());
    lcd_val(182, buf, BSP_LCD_COLOR_WHITE);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)DataLink_GetCmdLostCount());
    lcd_val(208, buf, BSP_LCD_COLOR_WHITE);
}

static void lcd_bus_draw(void)
{
    lcd_page_clear_content();
    lcd_page_header("BUS");
    BSP_LCD_ShowString(4, 30, "EVENT BUS", BSP_LCD_COLOR_YELLOW,
                       BSP_LCD_FONT_16);
    BSP_LCD_ShowString(8, 62, "Lost", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 88, "Queue/Pool", BSP_LCD_COLOR_LGRAY,
                       BSP_LCD_FONT_12);
    BSP_LCD_ShowString(4, 126, "HOSTLINK", BSP_LCD_COLOR_YELLOW,
                       BSP_LCD_FONT_16);
    BSP_LCD_ShowString(8, 158, "TX lost", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 184, "TX err", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 210, "Cmd lost", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    lcd_bus_refresh();
    lcd_page_footer("KEY: NEXT PAGE");
}

/* ================= 页面注册 ================= */
static const lcd_ui_page_t lcd_pages[] = {
    { "HOME",   lcd_home_draw,   lcd_home_refresh },
    { "SYSTEM", lcd_system_draw, lcd_system_refresh },
    { "BUS",    lcd_bus_draw,    lcd_bus_refresh },
};

/* ---------- 按键导航 ---------- */
static void lcd_on_key(const message_t *msg)
{
    if (msg != NULL && msg->hdr.type == MSG_KEY_SHORT) {
        LcdUI_NextPage();
    }
}

/* ---------- 初始化 ---------- */
void LcdApp_Init(void)
{
    uint16_t id = BSP_LCD_Init();
    LOG_Printf("[APP] LCD  : id=0x%04X, %ux%u\r\n",
               id, BSP_LCD_GetWidth(), BSP_LCD_GetHeight());

    LcdUI_Init();
    for (unsigned int i = 0; i < sizeof(lcd_pages) / sizeof(lcd_pages[0]); i++) {
        LcdUI_AddPage(&lcd_pages[i]);
    }
    LcdUI_ShowPage(0);
    EventBus_Subscribe(MSG_KEY_SHORT, lcd_on_key);
}
