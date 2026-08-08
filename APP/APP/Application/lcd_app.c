#include "lcd_app.h"
#include "bsp_lcd.h"
#include "lcd_ui.h"
#include "touch_svc.h"
#include "bsp_touch.h"
#include "imu_svc.h"
#include "eth_app.h"
#include "event_bus.h"
#include "app_config.h"
#include "data_link.h"
#include "FreeRTOS.h"
#include "task.h"
#include "logger.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

/* ================================================================
 * 板载 LCD 系统信息面板（基于 LcdUI 渲染任务框架）
 *   - HOME/SYSTEM/BUS 三页注册，按键切换，1s 周期刷新
 *   - 布局规范：标签左列 x=8（FONT12），数值右对齐 x=150（FONT16），
 *     行距 26px，数值与标签垂直居中对齐（FONT16 上移 2px）
 *   - SYSTEM 1s 刷新只重画任务列表（行内先清后画），不整页重绘 → 无频闪
 * ================================================================ */

#define LCD_MAX_TASKS    20
#define LCD_VAL_RIGHT    150   /* 数值右对齐边线（FONT16，8px/字符） */

static uint32_t lcd_run_prev = 0, lcd_idle_prev = 0;
static uint8_t  lcd_cpu_pct = 0;
static uint32_t lcd_cpu_last_ms = 0;
/* 任务状态缓冲：静态分配，避免每秒 pvPortMalloc 的堆抖动与碎片化。
 * 仅在 LcdUI 渲染任务内使用（各页 refresh 串行执行），无并发风险。 */
static TaskStatus_t s_lcd_tasks[LCD_MAX_TASKS];

/* SYSTEM 页任务列表：固定行高 + 触摸纵向滚动（仅渲染任务访问） */
#define SYS_ROWS_VISIBLE   12
#define SYS_ROW_H          20
#define SYS_LIST_Y0        40
#define SYS_LIST_Y1        (SYS_LIST_Y0 + SYS_ROWS_VISIBLE * SYS_ROW_H) /* 280 */
static int16_t s_sys_scroll = 0;       /* 已向下滚动的行数 */
static int16_t s_sys_scroll_last = -1; /* -1 = 首次强制整区重绘 */
static uint8_t s_sys_overflow = 0;     /* 任务数超过可视行数 */
static uint8_t s_sys_total = 0;        /* 当前任务总数 */

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

/* ---------- 数值（FONT16，右对齐至指定右边线） ---------- */
static void lcd_val_at(uint16_t xr, uint16_t y, const char *s, uint16_t color)
{
    uint16_t n = (uint16_t)strlen(s);
    BSP_LCD_ShowString((uint16_t)(xr - n * 8u), y, s, color, BSP_LCD_FONT_16);
}

static void lcd_val(uint16_t y, const char *s, uint16_t color)
{
    lcd_val_at(LCD_VAL_RIGHT, y, s, color);
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
    s_sys_total = (uint8_t)n;

    /* 固定排序：基础优先级降序 + 名称升序。
     * 用 uxBasePriority 而非 uxCurrentPriority：互斥量优先级继承会临时
     * 抬高持有者当前优先级，导致顺序每帧跳动；基础优先级恒稳定。 */
    for (UBaseType_t i = 1; i < n; i++) {
        TaskStatus_t key = s_lcd_tasks[i];
        int j = (int)i - 1;
        while (j >= 0 &&
               (s_lcd_tasks[j].uxBasePriority < key.uxBasePriority ||
                (s_lcd_tasks[j].uxBasePriority == key.uxBasePriority &&
                 strcmp(s_lcd_tasks[j].pcTaskName, key.pcTaskName) > 0))) {
            s_lcd_tasks[j + 1] = s_lcd_tasks[j];
            j--;
        }
        s_lcd_tasks[j + 1] = key;
    }

    /* 滚动范围钳位 */
    s_sys_overflow = (n > SYS_ROWS_VISIBLE);
    int16_t max_scroll = s_sys_overflow ? (int16_t)(n - SYS_ROWS_VISIBLE) : 0;
    if (s_sys_scroll > max_scroll) s_sys_scroll = max_scroll;
    if (s_sys_scroll < 0) s_sys_scroll = 0;

    /* 滚动位置变化 → 整区清一次，杜绝行残影 */
    if (s_sys_scroll != s_sys_scroll_last) {
        BSP_LCD_Fill(4, SYS_LIST_Y0, (uint16_t)(BSP_LCD_GetWidth() - 5),
                     (uint16_t)(SYS_LIST_Y1 - 1), BSP_LCD_COLOR_BLACK);
        s_sys_scroll_last = s_sys_scroll;
    }

    BSP_LCD_ShowString(4, 28, "TASK", BSP_LCD_COLOR_YELLOW, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(130, 28, "STACK", BSP_LCD_COLOR_YELLOW, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(210, 28, "PRIO", BSP_LCD_COLOR_YELLOW, BSP_LCD_FONT_12);

    for (uint8_t i = 0; i < SYS_ROWS_VISIBLE; i++) {
        uint16_t y = (uint16_t)(SYS_LIST_Y0 + i * SYS_ROW_H);
        /* 行内先清后画：只刷新本行，不整页重绘（无频闪） */
        BSP_LCD_Fill(4, y, (uint16_t)(BSP_LCD_GetWidth() - 5),
                     (uint16_t)(y + 15), BSP_LCD_COLOR_BLACK);
        int16_t idx = (int16_t)(s_sys_scroll + i);
        if (idx >= (int16_t)n) continue;   /* 滚动到底后清空剩余行 */
        BSP_LCD_ShowString(4, y, s_lcd_tasks[idx].pcTaskName,
                           BSP_LCD_COLOR_WHITE,
                           BSP_LCD_FONT_12);
        BSP_LCD_ShowNum(130, y, s_lcd_tasks[idx].usStackHighWaterMark, 4,
                        BSP_LCD_COLOR_GREEN, BSP_LCD_FONT_12);
        BSP_LCD_ShowNum(210, y, s_lcd_tasks[idx].uxBasePriority, 2,
                        BSP_LCD_COLOR_CYAN, BSP_LCD_FONT_12);
    }

    /* 右侧滚动条（仅溢出时显示）：轨道 + 比例滑块 */
    if (s_sys_overflow) {
        uint16_t track_h = (uint16_t)(SYS_LIST_Y1 - SYS_LIST_Y0);
        uint16_t thumb_h = (uint16_t)((uint32_t)SYS_ROWS_VISIBLE *
                                      track_h / n);
        if (thumb_h < 12) thumb_h = 12;
        uint16_t thumb_y = (uint16_t)(SYS_LIST_Y0 +
                          (uint32_t)s_sys_scroll * (track_h - thumb_h) /
                          max_scroll);
        BSP_LCD_Fill(235, SYS_LIST_Y0, 238, (uint16_t)(SYS_LIST_Y1 - 1),
                     BSP_LCD_COLOR_LGRAY);
        BSP_LCD_Fill(235, thumb_y, 238,
                     (uint16_t)(thumb_y + thumb_h - 1),
                     BSP_LCD_COLOR_WHITE);
    } else {
        BSP_LCD_Fill(235, SYS_LIST_Y0, 238, (uint16_t)(SYS_LIST_Y1 - 1),
                     BSP_LCD_COLOR_BLACK);
    }
}

/* SYSTEM 页触摸：纵向滑动滚动任务列表（渲染任务上下文，直接重绘） */
static void lcd_system_touch(uint8_t evt, uint16_t x, uint16_t y)
{
    (void)x;
    (void)y;
    int16_t max_scroll = s_sys_overflow
                             ? (int16_t)(s_sys_total - SYS_ROWS_VISIBLE)
                             : 0;
    if (evt == TOUCH_EVT_SWIPE_UP && s_sys_scroll < max_scroll) {
        s_sys_scroll++;
        lcd_system_list();
    } else if (evt == TOUCH_EVT_SWIPE_DOWN && s_sys_scroll > 0) {
        s_sys_scroll--;
        lcd_system_list();
    }
}

static void lcd_system_draw(void)
{
    lcd_page_clear_content();
    lcd_page_header("SYSTEM");
    s_sys_scroll = 0;          /* 进入页面从顶部开始 */
    s_sys_scroll_last = -1;
    lcd_system_list();
    lcd_page_footer(s_sys_overflow ? "SWIPE UP/DOWN: SCROLL | KEY: NEXT"
                                   : "KEY: NEXT PAGE");
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

/* ================= NET 页（以太网链路/IP/流量） ================= */
static void lcd_net_refresh(void)
{
    char buf[24];
    EthApp_RefreshStatus();
    const eth_status_t *st = EthApp_GetStatus();
    BSP_LCD_ShowString(8, 36, "Link", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(60, 36, st->link_up ? "UP" : "DOWN",
                       st->link_up ? BSP_LCD_COLOR_GREEN : BSP_LCD_COLOR_RED,
                       BSP_LCD_FONT_16);
    if (st->link_up) {
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                 st->ip[0], st->ip[1], st->ip[2], st->ip[3]);
    } else {
        snprintf(buf, sizeof(buf), "-.-.-.-");
    }
    lcd_val(92, buf, BSP_LCD_COLOR_CYAN);
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             st->mac[0], st->mac[1], st->mac[2],
             st->mac[3], st->mac[4], st->mac[5]);
    lcd_val(118, buf, BSP_LCD_COLOR_YELLOW);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)st->rx_packets);
    lcd_val(144, buf, BSP_LCD_COLOR_WHITE);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)st->tx_packets);
    lcd_val(170, buf, BSP_LCD_COLOR_WHITE);
    snprintf(buf, sizeof(buf), "%lus", (unsigned long)st->link_uptime_s);
    lcd_val(196, buf, BSP_LCD_COLOR_GREEN);
}

static void lcd_net_draw(void)
{
    lcd_page_clear_content();
    lcd_page_header("NET");
    BSP_LCD_ShowString(8, 92, "IP", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 118, "MAC", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 144, "RX", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 170, "TX", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 196, "Uptime", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    lcd_net_refresh();
    lcd_page_footer("SWIPE: NEXT PAGE");
}

/* ================= TOUCH 页（触摸测试/调校观察） ================= */
static const char *lcd_touch_state_name(uint8_t st)
{
    switch (st) {
    case TOUCH_EVT_DOWN: return "DOWN";
    case TOUCH_EVT_MOVE: return "MOVE";
    case TOUCH_EVT_UP:   return "UP";
    default:             return "NONE";
    }
}

static void lcd_touch_refresh(void)
{
    const touch_svc_state_t *ts = TouchSvc_GetState();
    bsp_touch_cal_t cal;
    char buf[24];
    BSP_Touch_GetCal(&cal);

    snprintf(buf, sizeof(buf), "%s", lcd_touch_state_name(ts->state));
    lcd_val(92, buf, BSP_LCD_COLOR_WHITE);
    snprintf(buf, sizeof(buf), "%u,%u", (unsigned)ts->x, (unsigned)ts->y);
    lcd_val(118, buf, BSP_LCD_COLOR_GREEN);
    snprintf(buf, sizeof(buf), "%u,%u", (unsigned)ts->raw_x, (unsigned)ts->raw_y);
    lcd_val(144, buf, BSP_LCD_COLOR_CYAN);
    snprintf(buf, sizeof(buf), "%ld,%ld", (long)cal.xfac, (long)cal.yfac);
    lcd_val(170, buf, BSP_LCD_COLOR_YELLOW);
    snprintf(buf, sizeof(buf), "%ld,%ld", (long)cal.xc, (long)cal.yc);
    lcd_val(196, buf, BSP_LCD_COLOR_YELLOW);
}

static void lcd_touch_draw(void)
{
    lcd_page_clear_content();
    lcd_page_header("TOUCH");
    BSP_LCD_ShowString(8, 36, "Touch Test", BSP_LCD_COLOR_YELLOW,
                       BSP_LCD_FONT_16);
    BSP_LCD_ShowString(8, 58, "Finger track / swipe page",
                       BSP_LCD_COLOR_CYAN, BSP_LCD_FONT_12);
    BSP_LCD_Fill(8, 80, (uint16_t)(BSP_LCD_GetWidth() - 9), 81,
                 BSP_LCD_COLOR_GRAY);

    BSP_LCD_ShowString(8, 94, "State", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 120, "Position", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 146, "Raw", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 172, "CalXY", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(8, 198, "Center", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    lcd_touch_refresh();
    lcd_page_footer("SWIPE: NEXT PAGE");
}

/* 触摸实时反馈：手指移动时刷新 Position（渲染任务上下文） */
static void lcd_touch_event(uint8_t evt, uint16_t x, uint16_t y)
{
    if (evt == TOUCH_EVT_DOWN || evt == TOUCH_EVT_MOVE) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%u,%u", (unsigned)x, (unsigned)y);
        lcd_val(118, buf, BSP_LCD_COLOR_GREEN);
    }
}

/* ================= IMU 页（3D 姿态线框立方体 + 坐标轴 + 实时数值） ================= */
#define IMU_CUBE_CX     52
#define IMU_CUBE_CY     112
#define IMU_CUBE_SIZE   28     /* 立方体半边长（像素），小体积避免覆盖文字 */
#define IMU_AXIS_LEN    36     /* 坐标轴长度（超出立方体便于观察） */

/* 立方体几何：8 顶点 / 12 边 / 6 面外法线 */
static const int8_t s_cube_verts[8][3] = {
    { -1, -1, -1 }, {  1, -1, -1 }, {  1,  1, -1 }, { -1,  1, -1 },
    { -1, -1,  1 }, {  1, -1,  1 }, {  1,  1,  1 }, { -1,  1,  1 },
};
static const uint8_t s_cube_edges[12][2] = {
    { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
    { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
};
/* 每条边相邻的两个面（用于可见性：任一面可见则画该边） */
static const uint8_t s_edge_faces[12][2] = {
    { 1, 5 }, { 1, 2 }, { 1, 4 }, { 1, 3 },
    { 0, 5 }, { 0, 2 }, { 0, 4 }, { 0, 3 },
    { 3, 5 }, { 2, 5 }, { 2, 4 }, { 3, 4 },
};
static const int8_t s_cube_normals[6][3] = {
    { 0, 0, 1 }, { 0, 0, -1 },
    { 1, 0, 0 }, { -1, 0, 0 },
    { 0, 1, 0 }, { 0, -1, 0 },
};
static float s_cube_q[4] = { 1.0f, 0.0f, 0.0f, 0.0f };

/* 四元数旋转向量（q ⊗ v ⊗ q*，优化公式） */
static void quat_rotate(float q0, float q1, float q2, float q3,
                        float vx, float vy, float vz,
                        float *ox, float *oy, float *oz)
{
    float tx = 2.0f * (q2 * vz - q3 * vy);
    float ty = 2.0f * (q3 * vx - q1 * vz);
    float tz = 2.0f * (q1 * vy - q2 * vx);
    float ux = q2 * tz - q3 * ty;
    float uy = q3 * tx - q1 * tz;
    float uz = q1 * ty - q2 * tx;
    *ox = vx + q0 * tx + ux;
    *oy = vy + q0 * ty + uy;
    *oz = vz + q0 * tz + uz;
}

/* 边界钳制画线（杜绝越界坐标导致画线循环失控） */
static void lcd_line_clamped(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (x0 < 0) x0 = 0;
    if (x0 > 239) x0 = 239;
    if (x1 < 0) x1 = 0;
    if (x1 > 239) x1 = 239;
    if (y0 < 0) y0 = 0;
    if (y0 > 319) y0 = 319;
    if (y1 < 0) y1 = 0;
    if (y1 > 319) y1 = 319;
    BSP_LCD_DrawLine((uint16_t)x0, (uint16_t)y0,
                     (uint16_t)x1, (uint16_t)y1, color);
}

/* 渲染姿态线框立方体 + 中心坐标轴（erase=1 擦旧为黑） */
static void lcd_imu_cube(const float q[4], uint8_t erase)
{
    float rx[8], ry[8], rz[8];
    uint8_t vis[6] = {0, 0, 0, 0, 0, 0};

    for (int i = 0; i < 8; i++) {
        quat_rotate(q[0], q[1], q[2], q[3],
                    (float)s_cube_verts[i][0], (float)s_cube_verts[i][1],
                    (float)s_cube_verts[i][2], &rx[i], &ry[i], &rz[i]);
    }
    /* 可见面判定（法线朝向观察者） */
    for (int f = 0; f < 6; f++) {
        float nx, ny, nz;
        quat_rotate(q[0], q[1], q[2], q[3],
                    (float)s_cube_normals[f][0],
                    (float)s_cube_normals[f][1],
                    (float)s_cube_normals[f][2], &nx, &ny, &nz);
        if (nz > 0.0f) vis[f] = 1;
    }

    /* 可见边（线框勾勒，隐藏边不画 → 立体感正确） */
    {
        uint16_t color = erase ? BSP_LCD_COLOR_BLACK : BSP_LCD_COLOR_WHITE;
        for (int e = 0; e < 12; e++) {
            if (!vis[s_edge_faces[e][0]] && !vis[s_edge_faces[e][1]]) continue;
            int v0 = s_cube_edges[e][0], v1 = s_cube_edges[e][1];
            lcd_line_clamped(
                IMU_CUBE_CX + (int)(rx[v0] * IMU_CUBE_SIZE),
                IMU_CUBE_CY - (int)(ry[v0] * IMU_CUBE_SIZE),
                IMU_CUBE_CX + (int)(rx[v1] * IMU_CUBE_SIZE),
                IMU_CUBE_CY - (int)(ry[v1] * IMU_CUBE_SIZE), color);
        }
    }

    /* 中心三维坐标轴（相对立方体静止，X红 Y绿 Z蓝） */
    {
        static const int8_t axis[3][3] = {
            { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 },
        };
        static const uint16_t axis_color[3] = {
            0xF800, 0x07E0, 0x001F,
        };
        for (int a = 0; a < 3; a++) {
            float dx, dy, dz;
            quat_rotate(q[0], q[1], q[2], q[3],
                        (float)axis[a][0], (float)axis[a][1],
                        (float)axis[a][2], &dx, &dy, &dz);
            uint16_t c = erase ? BSP_LCD_COLOR_BLACK : axis_color[a];
            int tx = IMU_CUBE_CX + (int)(dx * IMU_AXIS_LEN);
            int ty = IMU_CUBE_CY - (int)(dy * IMU_AXIS_LEN);
            lcd_line_clamped(IMU_CUBE_CX, IMU_CUBE_CY, tx, ty, c);
            /* 箭头：尖端 V 形两翼（屏幕方向 sx,sy + 垂直 px,py） */
            float sx = dx, sy = -dy;
            float px = -sy, py = sx;
            float L = 7.0f, W = 3.5f;
            int wx1 = tx + (int)(-sx * L + px * W);
            int wy1 = ty + (int)(-sy * L + py * W);
            int wx2 = tx + (int)(-sx * L - px * W);
            int wy2 = ty + (int)(-sy * L - py * W);
            lcd_line_clamped(tx, ty, wx1, wy1, c);
            lcd_line_clamped(tx, ty, wx2, wy2, c);
        }
    }
}

static void lcd_imu_refresh(void)
{
    const imu_svc_state_t *s = ImuSvc_GetState();
    char buf[32];
    static uint32_t s_imu_status_ms = 0;

    /* 快路径（100Hz）：姿态角实时更新 */
    snprintf(buf, sizeof(buf), "%+6.1fdeg", (double)s->roll);   /* 定宽 9 字符 */
    lcd_val_at(225, 92, buf, BSP_LCD_COLOR_WHITE);
    snprintf(buf, sizeof(buf), "%+6.1fdeg", (double)s->pitch);
    lcd_val_at(225, 118, buf, BSP_LCD_COLOR_WHITE);
    snprintf(buf, sizeof(buf), "%+6.1fdeg", (double)s->yaw);
    lcd_val_at(225, 144, buf, BSP_LCD_COLOR_YELLOW);

    /* 慢路径（10Hz）：A/G/T 状态行（诊断数据，无需高频） */
    {
        uint32_t now = (uint32_t)xTaskGetTickCount();
        if (now - s_imu_status_ms >= pdMS_TO_TICKS(100)) {
            s_imu_status_ms = now;
            snprintf(buf, sizeof(buf), "A %+6.2f %+6.2f %+6.2f g",
                     (double)s->ax, (double)s->ay, (double)s->az);
            BSP_LCD_ShowString(8, 168, buf, BSP_LCD_COLOR_CYAN, BSP_LCD_FONT_12);
            snprintf(buf, sizeof(buf), "G %+6.1f %+6.1f %+6.1f dps",
                     (double)s->gx, (double)s->gy, (double)s->gz);
            BSP_LCD_ShowString(8, 184, buf, BSP_LCD_COLOR_GREEN, BSP_LCD_FONT_12);
            snprintf(buf, sizeof(buf), "T %+6.1f C n=%lu", (double)s->temp,
                     (unsigned long)s->sample_count);
            BSP_LCD_ShowString(8, 200, buf, BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
        }
    }

    /* 3D 姿态立方体：姿态变化超过 ~0.2° 才擦旧画新（静止零闪烁） */
    {
        float q[4] = { s->q0, s->q1, s->q2, s->q3 };
        float dot = q[0] * s_cube_q[0] + q[1] * s_cube_q[1] +
                    q[2] * s_cube_q[2] + q[3] * s_cube_q[3];
        if (dot < 0.0f) dot = -dot;
        if (dot < 0.999994f) {  /* 约 0.2° 阈值（极致实时） */
            lcd_imu_cube(s_cube_q, 1);
            s_cube_q[0] = q[0]; s_cube_q[1] = q[1];
            s_cube_q[2] = q[2]; s_cube_q[3] = q[3];
            lcd_imu_cube(q, 0);
        }
    }
}

static void lcd_imu_draw(void)
{
    lcd_page_clear_content();
    lcd_page_header("IMU");
    BSP_LCD_ShowString(8, 36, "MPU6050 AHRS", BSP_LCD_COLOR_YELLOW,
                       BSP_LCD_FONT_16);
    BSP_LCD_ShowString(8, 56, "3D attitude cube", BSP_LCD_COLOR_CYAN,
                       BSP_LCD_FONT_12);

    BSP_LCD_ShowString(110, 94, "R", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(110, 120, "P", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);
    BSP_LCD_ShowString(110, 146, "Y", BSP_LCD_COLOR_LGRAY, BSP_LCD_FONT_12);

    s_cube_q[0] = 1.0f;
    s_cube_q[1] = s_cube_q[2] = s_cube_q[3] = 0.0f;
    lcd_imu_cube(s_cube_q, 0);
    lcd_imu_refresh();
    lcd_page_footer("SWIPE: NEXT PAGE");
}

/* ================= 页面注册 ================= */
static const lcd_ui_page_t lcd_pages[] = {
    { "HOME",   lcd_home_draw,   lcd_home_refresh,   NULL,            0 },
    { "SYSTEM", lcd_system_draw, lcd_system_refresh, lcd_system_touch, 0 },
    { "BUS",    lcd_bus_draw,    lcd_bus_refresh,    NULL,            0 },
    { "NET",    lcd_net_draw,    lcd_net_refresh,    NULL,            0 },
    { "TOUCH",  lcd_touch_draw,  lcd_touch_refresh,  lcd_touch_event, 0 },
    { "IMU",    lcd_imu_draw,    lcd_imu_refresh,    NULL,            10 },
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
