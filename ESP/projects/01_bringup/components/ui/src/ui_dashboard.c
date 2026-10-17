/* ================================================================
 * ui_dashboard.c —— 应用界面（320x240 仪表盘）
 *
 * 页面结构（每页独立 screen，切换用 lv_scr_load_anim 做滑动动画）：
 *   HOME  时钟 + 温度弧表 + 光照条 + IMU 状态灯
 *   IMU   三通道实时曲线（accX / accY / gyroZ）+ 数值
 *   SYS   内部 RAM / PSRAM 占用条 + BSP 模块在线清单 + FPS
 *
 * 底部常驻三页签；触摸点击或按键 K1/K2 都能切页。
 * ================================================================ */
#include "ui/ui.h"
#include "ui_internal.h"

#include "lvgl.h"

#include "bsp/bsp.h"
#include "bsp/imu.h"
#include "bsp/lightsensor.h"
#include "bsp/rtc.h"
#include "bsp/temperature.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "UI/UI";

/* ---------------- 配色（与 STM32 侧界面同一套审美） ---------------- */
#define COL_BG      lv_color_hex(0x0b1118)
#define COL_CARD    lv_color_hex(0x151d27)
#define COL_CARD2   lv_color_hex(0x1e2836)
#define COL_TEXT    lv_color_hex(0xe6edf3)
#define COL_DIM     lv_color_hex(0x8b9bb0)
#define COL_ACCENT  lv_color_hex(0x4ea1ff)
#define COL_GREEN   lv_color_hex(0x62d6a0)
#define COL_ORANGE  lv_color_hex(0xffb454)
#define COL_VIOLET  lv_color_hex(0xb48cff)
#define COL_RED     lv_color_hex(0xff6b7a)

/* 前置声明：导航定时器在 ui_dashboard_build() 里注册，定义在下方 */
static void nav_timer_cb(lv_timer_t *t);

#define PAGE_COUNT  3
#define TAB_H       34
#define TAB_Y       (BSP_LCD_HEIGHT - TAB_H)

/* ---------------- 页面与控件 ---------------- */
static lv_obj_t *s_scr[PAGE_COUNT];
static uint8_t   s_page;
static volatile int8_t s_pending_dir;    /* 跨任务切页请求：+1/-1/0 */

/* HOME */
static lv_obj_t *s_lbl_time;
static lv_obj_t *s_lbl_date;
static lv_obj_t *s_arc_temp;
static lv_obj_t *s_lbl_temp;
static lv_obj_t *s_bar_light;
static lv_obj_t *s_lbl_light;
static lv_obj_t *s_led_imu;
static lv_obj_t *s_lbl_imu;

/* IMU */
static lv_obj_t *s_chart;
static lv_chart_series_t *s_ser_ax;
static lv_chart_series_t *s_ser_ay;
static lv_chart_series_t *s_ser_gz;
static lv_obj_t *s_lbl_acc;
static lv_obj_t *s_lbl_gyro;

/* SYS */
static lv_obj_t *s_bar_ram;
static lv_obj_t *s_lbl_ram;
static lv_obj_t *s_bar_ps;
static lv_obj_t *s_lbl_ps;
static lv_obj_t *s_lbl_mod;
static lv_obj_t *s_lbl_fps;

/* ================================================================
 *                        小工具
 * ================================================================ */

/** 全屏基础样式：纯色背景、无边框、无滚动 */
static void base_screen(lv_obj_t *scr)
{
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
}

/** 圆角卡片 */
static lv_obj_t *card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                      lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(o, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, COL_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(o, 0, LV_PART_MAIN);      /* 关阴影：省抗锯齿合成 */
    lv_obj_set_style_pad_all(o, 6, LV_PART_MAIN);
    return o;
}

/** 文本标签 */
static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font,
                          lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    lv_label_set_text(l, txt != NULL ? txt : "");
    return l;
}

/* ---------------- 底部页签（每页各放一份，高亮当前页） ---------------- */
static void tab_bar_cb(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(lv_uintptr_t)lv_event_get_user_data(e);
    if (idx < PAGE_COUNT && idx != s_page) {
        s_page = idx;
        lv_scr_load_anim(s_scr[idx], LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, false);
    }
}

static void tab_bar(lv_obj_t *scr, lv_obj_t *tab_of_page[PAGE_COUNT], uint8_t self)
{
    static const char *const names[PAGE_COUNT] = { "HOME", "IMU", "SYS" };
    const lv_coord_t w = (BSP_LCD_WIDTH - 16) / PAGE_COUNT;

    for (uint8_t i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *b = lv_btn_create(scr);
        lv_obj_set_size(b, w - 4, TAB_H - 6);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 8 + i * w, -3);
        lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, (i == self) ? COL_ACCENT : COL_CARD, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);

        lv_obj_t *l = mk_label(b, &lv_font_montserrat_14,
                               (i == self) ? lv_color_hex(0x06121f) : COL_DIM, names[i]);
        lv_obj_center(l);

        lv_obj_add_event_cb(b, tab_bar_cb, LV_EVENT_CLICKED, (void *)(lv_uintptr_t)i);
        tab_of_page[i] = b;
    }
}

/* ================================================================
 *                        第 1 页：HOME
 * ================================================================ */
static void page_home_build(void)
{
    lv_obj_t *scr = s_scr[0];
    base_screen(scr);
    lv_obj_t *tab_of_page[PAGE_COUNT];

    /* 大号时钟 */
    s_lbl_time = mk_label(scr, &lv_font_montserrat_40, COL_TEXT, "--:--");
    lv_obj_align(s_lbl_time, LV_ALIGN_TOP_MID, 0, 6);

    s_lbl_date = mk_label(scr, &lv_font_montserrat_14, COL_DIM, "---- -- --");
    lv_obj_align_to(s_lbl_date, s_lbl_time, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    /* 温度弧表 */
    lv_obj_t *c1 = card(scr, 8, 96, 96, 102);
    s_arc_temp = lv_arc_create(c1);
    lv_obj_set_size(s_arc_temp, 74, 74);
    lv_obj_align(s_arc_temp, LV_ALIGN_TOP_MID, 0, 2);
    lv_arc_set_rotation(s_arc_temp, 135);
    lv_arc_set_bg_angles(s_arc_temp, 0, 270);
    lv_arc_set_range(s_arc_temp, 0, 600);           /* 0.0 ~ 60.0 ℃（0.1℃ 精度） */
    lv_arc_set_value(s_arc_temp, 250);
    lv_obj_remove_style(s_arc_temp, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc_temp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc_temp, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_temp, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc_temp, COL_CARD2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_temp, COL_ORANGE, LV_PART_INDICATOR);

    s_lbl_temp = mk_label(c1, &lv_font_montserrat_16, COL_ORANGE, "--.- C");
    lv_obj_align(s_lbl_temp, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* 光照条 */
    lv_obj_t *c2 = card(scr, 112, 96, 100, 102);
    lv_obj_t *t2 = mk_label(c2, &lv_font_montserrat_12, COL_DIM, "LIGHT");
    lv_obj_align(t2, LV_ALIGN_TOP_LEFT, 0, 0);
    s_bar_light = lv_bar_create(c2);
    lv_obj_set_size(s_bar_light, 84, 8);
    lv_obj_align(s_bar_light, LV_ALIGN_TOP_MID, 0, 22);
    lv_bar_set_range(s_bar_light, 0, 2000);
    lv_bar_set_value(s_bar_light, 600, LV_ANIM_ON);
    lv_obj_set_style_bg_color(s_bar_light, COL_CARD2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_light, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_light, 4, LV_PART_MAIN);
    s_lbl_light = mk_label(c2, &lv_font_montserrat_20, COL_ACCENT, "---");
    lv_obj_align(s_lbl_light, LV_ALIGN_BOTTOM_MID, 0, -2);

    /* IMU 状态灯 */
    lv_obj_t *c3 = card(scr, 220, 96, 92, 102);
    lv_obj_t *t3 = mk_label(c3, &lv_font_montserrat_12, COL_DIM, "IMU");
    lv_obj_align(t3, LV_ALIGN_TOP_LEFT, 0, 0);
    s_led_imu = lv_led_create(c3);
    lv_obj_set_size(s_led_imu, 20, 20);
    lv_obj_align(s_led_imu, LV_ALIGN_TOP_MID, 0, 20);
    lv_led_set_color(s_led_imu, COL_GREEN);
    lv_led_set_brightness(s_led_imu, 220);
    s_lbl_imu = mk_label(c3, &lv_font_montserrat_14, COL_GREEN, "OK");
    lv_obj_align(s_lbl_imu, LV_ALIGN_BOTTOM_MID, 0, -2);

    tab_bar(scr, tab_of_page, 0);
}

/* ================================================================
 *                        第 2 页：IMU
 * ================================================================ */
static void page_imu_build(void)
{
    lv_obj_t *scr = s_scr[1];
    base_screen(scr);
    lv_obj_t *tab_of_page[PAGE_COUNT];

    s_chart = lv_chart_create(scr);
    lv_obj_set_size(s_chart, BSP_LCD_WIDTH - 16, 136);
    lv_obj_align(s_chart, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, 60);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, -200, 200);
    lv_chart_set_div_line_count(s_chart, 4, 6);
    lv_obj_set_style_bg_color(s_chart, COL_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_chart, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(s_chart, 10, LV_PART_MAIN);
    lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR);      /* 不画数据点 */
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);

    s_ser_ax = lv_chart_add_series(s_chart, COL_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    s_ser_ay = lv_chart_add_series(s_chart, COL_GREEN, LV_CHART_AXIS_PRIMARY_Y);
    s_ser_gz = lv_chart_add_series(s_chart, COL_VIOLET, LV_CHART_AXIS_PRIMARY_Y);

    s_lbl_acc = mk_label(scr, &lv_font_montserrat_16, COL_TEXT, "ACC  --  --  --");
    lv_obj_align(s_lbl_acc, LV_ALIGN_TOP_LEFT, 12, 148);

    s_lbl_gyro = mk_label(scr, &lv_font_montserrat_16, COL_VIOLET, "GYRO --  --  --");
    lv_obj_align(s_lbl_gyro, LV_ALIGN_TOP_LEFT, 12, 170);

    tab_bar(scr, tab_of_page, 1);
}

/* ================================================================
 *                        第 3 页：SYS
 * ================================================================ */
static void page_sys_build(void)
{
    lv_obj_t *scr = s_scr[2];
    base_screen(scr);
    lv_obj_t *tab_of_page[PAGE_COUNT];

    lv_obj_t *c1 = card(scr, 8, 8, BSP_LCD_WIDTH - 16, 84);

    lv_obj_t *t1 = mk_label(c1, &lv_font_montserrat_12, COL_DIM, "INTERNAL RAM");
    lv_obj_align(t1, LV_ALIGN_TOP_LEFT, 0, 0);
    s_bar_ram = lv_bar_create(c1);
    lv_obj_set_size(s_bar_ram, BSP_LCD_WIDTH - 44, 10);
    lv_obj_align(s_bar_ram, LV_ALIGN_TOP_LEFT, 0, 18);
    lv_obj_set_style_bg_color(s_bar_ram, COL_CARD2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_ram, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_ram, 5, LV_PART_MAIN);
    s_lbl_ram = mk_label(c1, &lv_font_montserrat_12, COL_DIM, "--");
    lv_obj_align(s_lbl_ram, LV_ALIGN_RIGHT_MID, 0, -12);

    lv_obj_t *t2 = mk_label(c1, &lv_font_montserrat_12, COL_DIM, "PSRAM");
    lv_obj_align(t2, LV_ALIGN_TOP_LEFT, 0, 36);
    s_bar_ps = lv_bar_create(c1);
    lv_obj_set_size(s_bar_ps, BSP_LCD_WIDTH - 44, 10);
    lv_obj_align(s_bar_ps, LV_ALIGN_TOP_LEFT, 0, 54);
    lv_obj_set_style_bg_color(s_bar_ps, COL_CARD2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_ps, COL_VIOLET, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_ps, 5, LV_PART_MAIN);
    s_lbl_ps = mk_label(c1, &lv_font_montserrat_12, COL_DIM, "--");
    lv_obj_align(s_lbl_ps, LV_ALIGN_RIGHT_MID, 0, 32);

    lv_obj_t *c2 = card(scr, 8, 96, BSP_LCD_WIDTH - 16, 68);
    lv_obj_t *t3 = mk_label(c2, &lv_font_montserrat_12, COL_DIM, "BSP MODULES");
    lv_obj_align(t3, LV_ALIGN_TOP_LEFT, 0, 0);
    s_lbl_mod = mk_label(c2, &lv_font_montserrat_12, COL_TEXT, "--");
    lv_obj_set_width(s_lbl_mod, BSP_LCD_WIDTH - 40);
    lv_label_set_long_mode(s_lbl_mod, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_lbl_mod, LV_ALIGN_TOP_LEFT, 0, 16);

    lv_obj_t *c3 = card(scr, 8, 170, BSP_LCD_WIDTH - 16, 28);
    s_lbl_fps = mk_label(c3, &lv_font_montserrat_14, COL_ACCENT, "FPS --");
    lv_obj_center(s_lbl_fps);

    tab_bar(scr, tab_of_page, 2);
}

/* ---------------- 数据刷新定时器 ---------------- */
static void home_timer_cb(lv_timer_t *t)
{
    BSP_UNUSED(t);

    /* 时钟（RTC 未设置时显示占位符） */
    bsp_datetime_t dt;
    if (bsp_rtc_get(&dt) == ESP_OK) {
        lv_label_set_text_fmt(s_lbl_time, "%02u:%02u", dt.hour, dt.minute);
        lv_label_set_text_fmt(s_lbl_date, "%04u-%02u-%02u", dt.year, dt.month, dt.day);
    } else {
        lv_label_set_text(s_lbl_time, "--:--");
        lv_label_set_text(s_lbl_date, "---- -- --");
    }

    /* 片内温度（注意：Montserrat 字体不含 ℃/° 字形，用 "C" 避免每帧告警） */
    float c = 0.0f;
    if (bsp_temperature_read(&c) == ESP_OK) {
        int tenths = (int)(c * 10.0f);
        lv_arc_set_value(s_arc_temp, tenths);
        lv_label_set_text_fmt(s_lbl_temp, "%d.%d C", tenths / 10, (tenths % 10 + 10) % 10);
    }

    /* 光照 */
    uint16_t als = 0;
    if (bsp_light_read_als(&als) == ESP_OK) {
        lv_bar_set_value(s_bar_light, als > 2000 ? 2000 : als, LV_ANIM_ON);
        lv_label_set_text_fmt(s_lbl_light, "%u", als);
    }

    /* IMU 在线状态：读一次数据顺便当心跳 */
    bsp_imu_data_t imu;
    bool ok = (bsp_imu_read(&imu) == ESP_OK);
    lv_led_set_brightness(s_led_imu, ok ? 220 : 0);
    lv_label_set_text(s_lbl_imu, ok ? "OK" : "ERR");
    lv_obj_set_style_text_color(s_lbl_imu, ok ? COL_GREEN : COL_RED, LV_PART_MAIN);
}

static void imu_timer_cb(lv_timer_t *t)
{
    BSP_UNUSED(t);

    bsp_imu_data_t imu;
    if (bsp_imu_read(&imu) != ESP_OK) {
        return;
    }

    /* 曲线：acc 缩放 ×100（±2g → ±200），gyro 缩放 ×2（±100dps → ±200） */
    lv_chart_set_next_value(s_chart, s_ser_ax, (lv_coord_t)(imu.acc_g.x * 100.0f));
    lv_chart_set_next_value(s_chart, s_ser_ay, (lv_coord_t)(imu.acc_g.y * 100.0f));
    lv_chart_set_next_value(s_chart, s_ser_gz, (lv_coord_t)(imu.gyro_dps.z * 2.0f));

    lv_label_set_text_fmt(s_lbl_acc, "ACC  %+.2f %+.2f %+.2f g",
                          imu.acc_g.x, imu.acc_g.y, imu.acc_g.z);
    lv_label_set_text_fmt(s_lbl_gyro, "GYRO %+.1f %+.1f %+.1f dps",
                          imu.gyro_dps.x, imu.gyro_dps.y, imu.gyro_dps.z);
}

static void sys_timer_cb(lv_timer_t *t)
{
    BSP_UNUSED(t);

    /* 内部 RAM */
    size_t ram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    if (ram_total > 0) {
        int used_pct = (int)(100 - (ram_free * 100) / ram_total);
        lv_bar_set_value(s_bar_ram, used_pct, LV_ANIM_ON);
        lv_label_set_text_fmt(s_lbl_ram, "%u/%u KB", (unsigned)((ram_total - ram_free) / 1024),
                              (unsigned)(ram_total / 1024));
    }

    /* PSRAM */
    size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (ps_total > 0) {
        int used_pct = (int)(100 - (ps_free * 100) / ps_total);
        lv_bar_set_value(s_bar_ps, used_pct, LV_ANIM_ON);
        lv_label_set_text_fmt(s_lbl_ps, "%u/%u KB", (unsigned)((ps_total - ps_free) / 1024),
                              (unsigned)(ps_total / 1024));
    } else {
        lv_label_set_text(s_lbl_ps, "none");
    }

    /* 在线模块清单 */
    static const char *const names[17] = {
        "i2c", "spi", "xl9555", "eeprom", "light", "imu", "temp", "rtc", "adc",
        "uart", "led", "beep", "lcd", "touch", "key", "audio", "tfcard"
    };
    char buf[192];
    int n = 0;
    bsp_module_mask_t mask = bsp_modules_running();
    for (int i = 0; i < 17; i++) {
        if (mask & (1u << i)) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s ", names[i]);
            if (n >= (int)sizeof(buf) - 2) {
                break;
            }
        }
    }
    lv_label_set_text(s_lbl_mod, n > 0 ? buf : "--");

    lv_label_set_text_fmt(s_lbl_fps, "FPS %u   PSRAM %u MB",
                          (unsigned)ui_get_fps(), (unsigned)(ps_total / (1024 * 1024)));
}

/* ================================================================
 *                        构建与切页
 * ================================================================ */
void ui_dashboard_build(void)
{
    for (uint8_t i = 0; i < PAGE_COUNT; i++) {
        s_scr[i] = lv_obj_create(NULL);
        base_screen(s_scr[i]);
    }

    page_home_build();
    page_imu_build();
    page_sys_build();

    s_page = 0;
    lv_scr_load(s_scr[0]);

    /* 三个页面的数据刷新各自独立：不在当前页时定时器照跑，
     * 但控件不可见 → LVGL 不会重绘，CPU 开销可忽略 */
    lv_timer_create(home_timer_cb, 250, NULL);
    lv_timer_create(imu_timer_cb, 100, NULL);
    lv_timer_create(sys_timer_cb, 1000, NULL);
    lv_timer_create(nav_timer_cb, 40, NULL);       /* 按键请求 → LVGL 上下文执行 */

    ESP_LOGI(TAG, "dashboard built: %d pages", PAGE_COUNT);
}

void ui_dashboard_switch(int8_t dir)
{
    int8_t next = (int8_t)s_page + dir;
    if (next >= (int8_t)PAGE_COUNT) {
        next = 0;
    }
    if (next < 0) {
        next = (int8_t)PAGE_COUNT - 1;
    }
    if (next == (int8_t)s_page) {
        return;
    }
    s_page = (uint8_t)next;
    lv_scr_load_anim(s_scr[s_page],
                     (dir > 0) ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                     220, 0, false);
}

void ui_dashboard_request(int8_t dir)
{
    s_pending_dir = dir;                 /* 单字节标志，跨任务安全 */
}

/** 导航消费定时器：把"别的任务请求的切页"安全地搬到 LVGL 任务里执行 */
static void nav_timer_cb(lv_timer_t *t)
{
    BSP_UNUSED(t);
    int8_t dir = s_pending_dir;
    if (dir != 0) {
        s_pending_dir = 0;
        ui_dashboard_switch(dir);
    }
}

uint8_t ui_dashboard_page(void)
{
    return s_page;
}
