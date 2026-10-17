/* ================================================================
 * ui_dashboard.c —— 应用界面（320x240 / 象牙白 + 香槟金 奢侈表盘风）
 *
 * 设计语言（目标：奢华、尊贵、优雅；观感要"亮"，不能"暗"）：
 *   · 底色：暖象牙白 #F7F1E6 —— 高端表盘/信纸质感，比冷黑更贵气也更亮；
 *   · 强调：香槟金 #B08A2E（仅用于强调与刻度），数据辅色用深祖母绿 #2E6B5A；
 *   · 文字：浓咖啡 #2A2118 作主色（高对比、显精致），沙棕 #A08C6B 作标签；
 *   · 结构：内嵌 1px 沙金细框（像装裱的画框/表圈）＋ 细线分区，不用阴影；
 *   · 层级：40px 时间作主角，28px 数值，12px 大写+字距作标签（cap）；
 *   · 数值面板：米白面板 + 细沙边框，把三项指标"装"起来，视觉更安定；
 *   · 性能：全部纯色填充，无阴影/无渐变/无大字号字距慢路径，整屏仍在总线极限。
 *
 * 数据稳定性（本次重点修）：
 *   ① 滑动平均（EMA）压掉传感器噪声；
 *   ② 死区（滞回）——只有变化超过阈值才更新，避免末位来回跳；
 *   ③ 显示值没变就不刷新字符串（减少重绘）；
 *   ④ 补间从"当前显示值"出发（旧版每次从 0 开始，视觉上就是在乱跳）。
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "UI/UI";

/* ---------------- 调色板（象牙白 + 香槟金） ---------------- */
#define C_BG        lv_color_hex(0xF7F1E6)      /* 暖象牙白底 */
#define C_PANEL     lv_color_hex(0xFFFCF5)      /* 面板：比底色更亮一点 */
#define C_FRAME     lv_color_hex(0xDCCBA6)      /* 沙金细框 / 分区线 */
#define C_TEXT      lv_color_hex(0x2A2118)      /* 主文字：浓咖啡 */
#define C_SUB       lv_color_hex(0x6B5B45)      /* 次文字 */
#define C_FAINT     lv_color_hex(0xA08C6B)      /* 标签 / 单位：沙棕 */
#define C_GOLD      lv_color_hex(0xB08A2E)      /* 香槟金：强调 */
#define C_EMER      lv_color_hex(0x2E6B5A)      /* 祖母绿：数据辅助 */
#define C_TERRA     lv_color_hex(0xA8523A)      /* 陶土红：第三路数据 */
#define C_OK        lv_color_hex(0x3E8B63)
#define C_WARN      lv_color_hex(0xB07A2A)
#define C_ERR       lv_color_hex(0xA8452F)

#define PAGE_COUNT  3
#define TAB_Y       204
#define TAB_H       (BSP_LCD_HEIGHT - TAB_Y)
#define PAD         20
#define CONTENT_W   (BSP_LCD_WIDTH - PAD * 2)

/* ---------------- 状态 ---------------- */
static lv_obj_t *s_scr[PAGE_COUNT];
static uint8_t   s_page;
static volatile int8_t s_pending_dir;

/* 页签（每页各一份，用于高亮同步） */
static lv_obj_t *s_tab_lbl[PAGE_COUNT][PAGE_COUNT];
static lv_obj_t *s_tab_und[PAGE_COUNT][PAGE_COUNT];

/* HOME */
static lv_obj_t *s_lbl_time;
static lv_obj_t *s_lbl_date;
static lv_obj_t *s_val_temp;
static lv_obj_t *s_val_lux;
static lv_obj_t *s_val_motion;
static lv_obj_t *s_dot_motion;

/* MOTION */
static lv_obj_t *s_chart;
static lv_chart_series_t *s_ser[3];
static lv_obj_t *s_val_acc;
static lv_obj_t *s_val_gyro;

/* SYSTEM */
static lv_obj_t *s_bar_ram;
static lv_obj_t *s_val_ram;
static lv_obj_t *s_bar_ps;
static lv_obj_t *s_val_ps;
static lv_obj_t *s_val_fps;
static lv_obj_t *s_val_mod;

static void nav_timer_cb(lv_timer_t *t);

/* ================================================================
 *                        基础绘制件
 * ================================================================ */

/** 基础容器（可控底色/边框/圆角；透明传 LV_OPA_TRANSP） */
static lv_obj_t *mk_obj(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h, lv_color_t bg, lv_opa_t opa,
                        lv_color_t border, lv_coord_t border_w, lv_coord_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(o, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, opa, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, border, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, border_w, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(o, 0, LV_PART_MAIN);
    return o;
}

/** 细线（分隔/刻度） */
static void mk_rule(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                    lv_color_t color)
{
    lv_obj_t *o = mk_obj(parent, x, y, w, 1, color, LV_OPA_COVER, color, 0, 0);
    BSP_UNUSED(o);
}

/** 文本 */
static lv_obj_t *mk_text(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                         const lv_font_t *font, lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    lv_label_set_text(l, txt != NULL ? txt : "");
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    return l;
}

/** 小号大写标签 + 字距（仅小字号用；大字号加字距会走进排版慢路径） */
static lv_obj_t *mk_cap(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, const char *txt)
{
    lv_obj_t *l = mk_text(parent, x, y, &lv_font_montserrat_12, C_FAINT, txt);
    lv_obj_set_style_text_letter_space(l, 2, LV_PART_MAIN);
    return l;
}

/** 小圆点（状态指示 / 金色装饰） */
static lv_obj_t *mk_dot(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t d,
                        lv_color_t color)
{
    return mk_obj(parent, x, y, d, d, color, LV_OPA_COVER, color, 0, (lv_coord_t)(d / 2));
}

/** 细轨道（4px） */
static lv_obj_t *mk_bar(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_color_t color)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_size(b, w, 4);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, y);
    lv_bar_set_range(b, 0, 100);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, C_FRAME, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, color, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 2, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    return b;
}

/* ================================================================
 *                     数值补间（从当前值出发）
 * ================================================================ */
typedef struct {
    lv_obj_t   *label;
    const char *suffix;
    uint8_t     decimals;
} num_tween_t;

static void num_tween_apply(num_tween_t *t, int32_t v)
{
    if (t->label == NULL) {
        return;
    }
    if (t->decimals == 0U) {
        lv_label_set_text_fmt(t->label, "%ld%s", (long)v, t->suffix);
    } else {
        lv_label_set_text_fmt(t->label, "%ld.%01ld%s", (long)(v / 10), (long)(v % 10), t->suffix);
    }
}

static void num_tween_cb(void *var, int32_t v)
{
    num_tween_apply((num_tween_t *)var, v);
}

/**
 * @brief  个位数显（关键：from 传"当前显示值"，不再每次从 0 开始）
 * @param  shown  调用者的"当前显示值"（单位同 to；<0 表示首帧）
 */
static void num_set(lv_obj_t *label, int32_t *shown, int32_t to,
                    uint8_t decimals, const char *suffix, uint32_t ms)
{
    static num_tween_t s_slots[4];
    static uint8_t s_slot = 0;

    if (shown != NULL && *shown == to) {
        return;                       /* 显示值没变：不刷新、不重绘 */
    }
    int32_t from = (shown != NULL && *shown >= 0) ? *shown : to;
    if (shown != NULL) {
        *shown = to;
    }

    num_tween_t *t = &s_slots[s_slot];
    s_slot = (uint8_t)((s_slot + 1U) % 4U);
    t->label    = label;
    t->suffix   = suffix;
    t->decimals = decimals;

    if (from == to) {
        num_tween_apply(t, to);
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, num_tween_cb);
    lv_anim_start(&a);
    num_tween_apply(t, to);           /* 终值先落地，动画只做视觉修饰 */
}

/* ================================================================
 *                        顶部 / 底部常驻区
 * ================================================================ */
static void chrome_build(lv_obj_t *scr)
{
    /* 内嵌画框：像装裱的画/表圈，是"贵气"最有效的结构性细节 */
    lv_obj_t *frame = mk_obj(scr, 6, 6, BSP_LCD_WIDTH - 12, BSP_LCD_HEIGHT - 12,
                             C_BG, LV_OPA_TRANSP, C_FRAME, 1, 14);
    BSP_UNUSED(frame);

    mk_dot(scr, PAD, 15, 5, C_GOLD);
    lv_obj_t *brand = mk_text(scr, PAD + 10, 9, &lv_font_montserrat_12, C_SUB, "AI BOX");
    lv_obj_set_style_text_letter_space(brand, 2, LV_PART_MAIN);

    mk_rule(scr, PAD, 30, CONTENT_W, C_FRAME);       /* 页眉下的沙金细线 */
    mk_rule(scr, 0, TAB_Y - 1, BSP_LCD_WIDTH, C_FRAME);
}

static void tab_click_cb(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(lv_uintptr_t)lv_event_get_user_data(e);
    if (idx < PAGE_COUNT && idx != s_page) {
        ui_dashboard_switch((idx > s_page) ? 1 : -1);
    }
}

static void tabs_build(lv_obj_t *scr, uint8_t self)
{
    static const char *const names[PAGE_COUNT] = { "HOME", "MOTION", "SYSTEM" };
    const lv_coord_t step = BSP_LCD_WIDTH / PAGE_COUNT;
    const lv_coord_t centers[PAGE_COUNT] = {
        (lv_coord_t)(step / 2),
        (lv_coord_t)(step + step / 2),
        (lv_coord_t)(step * 2 + step / 2),
    };

    for (uint8_t i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *hit = mk_obj(scr, (lv_coord_t)(i * step), TAB_Y, step, TAB_H,
                               C_BG, LV_OPA_TRANSP, C_BG, 0, 0);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hit, tab_click_cb, LV_EVENT_CLICKED, (void *)(lv_uintptr_t)i);

        lv_obj_t *l = lv_label_create(scr);
        lv_label_set_text(l, names[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(l, 1, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, (i == self) ? C_TEXT : C_FAINT, LV_PART_MAIN);
        lv_obj_align(l, LV_ALIGN_TOP_MID,
                     (lv_coord_t)(centers[i] - BSP_LCD_WIDTH / 2), TAB_Y + 7);

        lv_obj_t *u = mk_obj(scr, (lv_coord_t)(centers[i] - 13), TAB_Y + 22, 26, 2,
                             (i == self) ? C_GOLD : C_BG,
                             (i == self) ? LV_OPA_COVER : LV_OPA_TRANSP, C_GOLD, 0, 1);

        s_tab_lbl[self][i] = l;
        s_tab_und[self][i] = u;
    }
}

/* ================================================================
 *                        第 1 页：HOME
 * ================================================================ */
static void page_home_build(void)
{
    lv_obj_t *scr = s_scr[0];

    chrome_build(scr);

    /* 主角：时间（浓咖啡大字，象牙底上对比极强） */
    s_lbl_time = mk_text(scr, PAD, 36, &lv_font_montserrat_40, C_TEXT, "--:--");
    s_lbl_date = mk_text(scr, PAD + 2, 84, &lv_font_montserrat_12, C_FAINT, "---- - -- - --");

    /* 数值面板：米白底 + 细沙边框，把三项指标"装"起来 */
    lv_obj_t *panel = mk_obj(scr, PAD, 108, CONTENT_W, 88, C_PANEL, LV_OPA_COVER, C_FRAME, 1, 12);
    BSP_UNUSED(panel);

    /* 三栏分隔（竖细线，比空着更有秩序感） */
    mk_rule(scr, PAD + 90, 118, 1, C_FRAME);
    mk_rule(scr, PAD + 180, 118, 1, C_FRAME);

    /* TEMP：金数值 + 沙棕单位 */
    mk_cap(scr, PAD + 14, 118, "TEMP");
    s_val_temp = mk_text(scr, PAD + 14, 134, &lv_font_montserrat_28, C_TEXT, "--.-");
    mk_text(scr, PAD + 76, 148, &lv_font_montserrat_12, C_GOLD, "C");

    /* LUX */
    mk_cap(scr, PAD + 104, 118, "LUX");
    s_val_lux = mk_text(scr, PAD + 104, 134, &lv_font_montserrat_28, C_TEXT, "----");

    /* MOTION：状态点 + 文字 */
    mk_cap(scr, PAD + 194, 118, "MOTION");
    s_dot_motion = mk_dot(scr, PAD + 194, 140, 7, C_OK);
    s_val_motion = mk_text(scr, PAD + 194, 152, &lv_font_montserrat_14, C_SUB, "steady");

    tabs_build(scr, 0);
}

/* ================================================================
 *                        第 2 页：MOTION
 * ================================================================ */
static void page_motion_build(void)
{
    lv_obj_t *scr = s_scr[1];

    chrome_build(scr);
    mk_cap(scr, PAD, 40, "MOTION VECTOR");

    s_chart = lv_chart_create(scr);
    lv_obj_set_size(s_chart, CONTENT_W, 110);
    lv_obj_align(s_chart, LV_ALIGN_TOP_LEFT, PAD, 58);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, 64);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, -200, 200);
    lv_chart_set_div_line_count(s_chart, 4, 8);
    /* 透明底 + 沙金极淡网格：曲线像画在纸上 */
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_chart, C_FRAME, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR);

    s_ser[0] = lv_chart_add_series(s_chart, C_EMER,  LV_CHART_AXIS_PRIMARY_Y);
    s_ser[1] = lv_chart_add_series(s_chart, C_GOLD,  LV_CHART_AXIS_PRIMARY_Y);
    s_ser[2] = lv_chart_add_series(s_chart, C_TERRA, LV_CHART_AXIS_PRIMARY_Y);

    mk_rule(scr, PAD, 174, CONTENT_W, C_FRAME);

    mk_cap(scr, PAD, 182, "ACC G");
    s_val_acc = mk_text(scr, PAD, 194, &lv_font_montserrat_14, C_SUB, "--  --  --");

    mk_cap(scr, PAD + 160, 182, "GYRO DPS");
    s_val_gyro = mk_text(scr, PAD + 160, 194, &lv_font_montserrat_14, C_SUB, "--  --  --");

    tabs_build(scr, 1);
}

/* ================================================================
 *                        第 3 页：SYSTEM
 * ================================================================ */
static void page_system_build(void)
{
    lv_obj_t *scr = s_scr[2];
    const lv_coord_t val_x = PAD + CONTENT_W - 150;

    chrome_build(scr);
    mk_cap(scr, PAD, 40, "MEMORY");

    mk_text(scr, PAD, 58, &lv_font_montserrat_12, C_SUB, "Internal");
    s_val_ram = mk_text(scr, val_x, 58, &lv_font_montserrat_12, C_FAINT, "--");
    lv_obj_set_style_width(s_val_ram, 150, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_val_ram, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    s_bar_ram = mk_bar(scr, PAD, 76, CONTENT_W, C_GOLD);

    mk_text(scr, PAD, 92, &lv_font_montserrat_12, C_SUB, "PSRAM");
    s_val_ps = mk_text(scr, val_x, 92, &lv_font_montserrat_12, C_FAINT, "--");
    lv_obj_set_style_width(s_val_ps, 150, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_val_ps, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    s_bar_ps = mk_bar(scr, PAD, 110, CONTENT_W, C_EMER);

    mk_rule(scr, PAD, 128, CONTENT_W, C_FRAME);

    mk_cap(scr, PAD, 136, "SYSTEM");
    s_val_fps = mk_text(scr, PAD, 152, &lv_font_montserrat_14, C_SUB, "fps --");
    s_val_mod = mk_text(scr, PAD, 174, &lv_font_montserrat_12, C_FAINT, "--");
    lv_obj_set_style_width(s_val_mod, CONTENT_W, LV_PART_MAIN);
    lv_label_set_long_mode(s_val_mod, LV_LABEL_LONG_WRAP);

    tabs_build(scr, 2);
}

/* ================================================================
 *                        数据刷新（含平滑与死区）
 * ================================================================ */
#define TEMP_EMA_ALPHA      0.12f    /* 温度：慢跟随（时间常数约 2s） */
#define TEMP_DEADBAND       0.25f    /* 温度死区：0.25℃ 内的抖动不显示（末位不跳） */
#define LUX_EMA_ALPHA       0.12f    /* 光照：更慢跟随，抗闪 */
#define LUX_DEADBAND_REL    0.04f    /* 光照死区：变化小于 4% 不更新 */

static float   s_temp_f  = 0.0f;
static int32_t s_temp_shown = -1;    /* 当前显示值（0.1℃） */
static float   s_lux_f   = 0.0f;
static int32_t s_lux_shown = -1;
static bool    s_smooth_init = false;

static void home_timer_cb(lv_timer_t *t)
{
    BSP_UNUSED(t);

    /* ---- 时间 ---- */
    bsp_datetime_t dt;
    if (bsp_rtc_get(&dt) == ESP_OK) {
        static uint8_t last_min = 0xFFU;
        static uint16_t last_day = 0xFFFFU;
        uint16_t day = (uint16_t)(dt.year * 1000U + dt.month * 40U + dt.day);
        if (dt.minute != last_min) {
            last_min = dt.minute;
            lv_label_set_text_fmt(s_lbl_time, "%02u:%02u", dt.hour, dt.minute);
        }
        if (day != last_day) {
            last_day = day;
            lv_label_set_text_fmt(s_lbl_date, "%04u - %02u - %02u", dt.year, dt.month, dt.day);
        }
    }

    /* ---- 温度：EMA + 死区 + 只在变化时补间 ---- */
    float c = 0.0f;
    if (bsp_temperature_read(&c) == ESP_OK) {
        if (!s_smooth_init) {
            s_temp_f = c;
        } else {
            s_temp_f += (c - s_temp_f) * TEMP_EMA_ALPHA;
        }
        int32_t tenths = (int32_t)(s_temp_f * 10.0f + ((s_temp_f >= 0.0f) ? 0.5f : -0.5f));
        if (tenths < 0) {
            tenths = 0;
        } else if (tenths > 900) {
            tenths = 900;
        }
        /* 死区：与已显示值差不到 0.15℃ 就当没变，避免末位来回跳 */
        if ((s_temp_shown < 0) ||
            (abs(tenths - s_temp_shown) >= (int32_t)(TEMP_DEADBAND * 10.0f))) {
            num_set(s_val_temp, &s_temp_shown, tenths, 1, "", 300);
        }
    }

    /* ---- 光照：EMA + 相对死区 + 量化到 10 ---- */
    uint16_t als = 0;
    if (bsp_light_read_als(&als) == ESP_OK) {
        if (!s_smooth_init) {
            s_lux_f = (float)als;
        } else {
            s_lux_f += ((float)als - s_lux_f) * LUX_EMA_ALPHA;
        }
        /* 量化到 10，进一步压掉末位跳动 */
        int32_t lux = (int32_t)((s_lux_f + 5.0f) / 10.0f) * 10;
        if (lux < 0) {
            lux = 0;
        } else if (lux > 99990) {
            lux = 99990;
        }
        bool changed = (s_lux_shown < 0) ||
                       (abs(lux - s_lux_shown) >=
                        (int32_t)((float)(s_lux_shown > 0 ? s_lux_shown : 10) * LUX_DEADBAND_REL));
        if (changed) {
            num_set(s_val_lux, &s_lux_shown, lux, 0, "", 300);
        }
    }
    s_smooth_init = true;

    /* ---- 姿态状态：加滞回，避免临界点来回闪 ---- */
    bsp_imu_data_t imu;
    bool ok = (bsp_imu_read(&imu) == ESP_OK);
    float mag2 = ok ? (imu.acc_g.x * imu.acc_g.x + imu.acc_g.y * imu.acc_g.y +
                       imu.acc_g.z * imu.acc_g.z) : 0.0f;
    static bool last_moving = false;
    bool moving = last_moving;
    if (ok) {
        if (mag2 > 1.45f) {
            moving = true;
        } else if (mag2 < 0.75f) {
            moving = true;
        } else if (mag2 > 0.85f && mag2 < 1.25f) {
            moving = false;                       /* 只有明显回到静止区才转回 */
        }
        last_moving = moving;
    }
    /* 只有"状态"真的变了才刷：状态灯与文字都按显示值做差分 */
    static int8_t last_state = -1;              /* -1 未初始化；0 静止；1 运动；2 不可用 */
    int8_t state = !ok ? 2 : (moving ? 1 : 0);
    if (state != last_state) {
        last_state = state;
        lv_obj_set_style_bg_color(s_dot_motion,
                                  (state == 2) ? C_ERR : (state == 1) ? C_WARN : C_OK,
                                  LV_PART_MAIN);
        lv_label_set_text(s_val_motion,
                          (state == 2) ? "n/a" : (state == 1) ? "moving" : "steady");
        lv_obj_set_style_text_color(s_val_motion, (state == 2) ? C_ERR : C_SUB, LV_PART_MAIN);
    }
}

static void motion_timer_cb(lv_timer_t *t)
{
    BSP_UNUSED(t);

    bsp_imu_data_t imu;
    if (bsp_imu_read(&imu) != ESP_OK) {
        return;
    }
    lv_chart_set_next_value(s_chart, s_ser[0], (lv_coord_t)(imu.acc_g.x * 100.0f));
    lv_chart_set_next_value(s_chart, s_ser[1], (lv_coord_t)(imu.acc_g.y * 100.0f));
    lv_chart_set_next_value(s_chart, s_ser[2], (lv_coord_t)(imu.gyro_dps.z * 2.0f));

    lv_label_set_text_fmt(s_val_acc, "%+.2f  %+.2f  %+.2f",
                          imu.acc_g.x, imu.acc_g.y, imu.acc_g.z);
    lv_label_set_text_fmt(s_val_gyro, "%+.0f  %+.0f  %+.0f",
                          imu.gyro_dps.x, imu.gyro_dps.y, imu.gyro_dps.z);
}

static void system_timer_cb(lv_timer_t *t)
{
    BSP_UNUSED(t);

    size_t ram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    if (ram_total > 0U) {
        lv_bar_set_value(s_bar_ram, (int32_t)(100U - (ram_free * 100U) / ram_total), LV_ANIM_OFF);
        lv_label_set_text_fmt(s_val_ram, "%u / %u K",
                              (unsigned)((ram_total - ram_free) / 1024U),
                              (unsigned)(ram_total / 1024U));
    }

    size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (ps_total > 0U) {
        lv_bar_set_value(s_bar_ps, (int32_t)(100U - (ps_free * 100U) / ps_total), LV_ANIM_OFF);
        lv_label_set_text_fmt(s_val_ps, "%u / %u K",
                              (unsigned)((ps_total - ps_free) / 1024U),
                              (unsigned)(ps_total / 1024U));
    }

    lv_label_set_text_fmt(s_val_fps, "%lu fps / %u M free",
                          (unsigned long)ui_get_fps(),
                          (unsigned)(ps_free / (1024U * 1024U)));

    static const char *const names[17] = {
        "i2c", "spi", "io", "eeprom", "light", "imu", "temp", "rtc", "adc",
        "uart", "led", "beep", "lcd", "touch", "key", "audio", "sd"
    };
    char buf[160];
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
    lv_label_set_text(s_val_mod, n > 0 ? buf : "--");
}

/** 页签高亮同步（每页各有一份页签） */
static void sync_tabs(void)
{
    for (uint8_t p = 0; p < PAGE_COUNT; p++) {
        for (uint8_t i = 0; i < PAGE_COUNT; i++) {
            if (s_tab_lbl[p][i] == NULL) {
                continue;
            }
            bool active = (i == s_page);
            lv_obj_set_style_text_color(s_tab_lbl[p][i], active ? C_TEXT : C_FAINT, LV_PART_MAIN);
            if (s_tab_und[p][i] != NULL) {
                lv_obj_set_style_bg_color(s_tab_und[p][i], active ? C_GOLD : C_BG, LV_PART_MAIN);
                lv_obj_set_style_bg_opa(s_tab_und[p][i],
                                        active ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
            }
        }
    }
}

/* ================================================================
 *                        构建与切页
 * ================================================================ */
void ui_dashboard_build(void)
{
    for (uint8_t i = 0; i < PAGE_COUNT; i++) {
        s_scr[i] = lv_obj_create(NULL);
        lv_obj_clear_flag(s_scr[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(s_scr[i], C_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_scr[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_scr[i], 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_scr[i], 0, LV_PART_MAIN);
    }

    page_home_build();
    page_motion_build();
    page_system_build();

    s_page = 0;
    sync_tabs();
    lv_scr_load(s_scr[0]);

    lv_timer_create(home_timer_cb, 250, NULL);
    lv_timer_create(motion_timer_cb, 100, NULL);
    lv_timer_create(system_timer_cb, 1000, NULL);
    lv_timer_create(nav_timer_cb, 40, NULL);

    ESP_LOGI(TAG, "dashboard built: %d pages (ivory + champagne gold)", PAGE_COUNT);
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
    sync_tabs();
    lv_scr_load_anim(s_scr[s_page],
                     (dir > 0) ? LV_SCR_LOAD_ANIM_OVER_LEFT : LV_SCR_LOAD_ANIM_OVER_RIGHT,
                     240, 0, false);
}

void ui_dashboard_request(int8_t dir)
{
    s_pending_dir = dir;
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
