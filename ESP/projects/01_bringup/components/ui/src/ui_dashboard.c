/* ================================================================
 * ui_dashboard.c —— 应用界面（320x240 精致仪表盘）
 *
 * 设计语言（目标：顶级优雅，而非"开发板的仪表盘"）：
 *   · 色彩：近黑底 + 单一香槟金强调色 + 冷蓝辅助；其余全是灰阶。
 *           颜色只用来标注"这是重点"，不参与装饰。
 *   · 结构：8px 网格对齐；主次靠字号与灰度分层，不靠色块。
 *   · 分隔：1px 细线（hairline）代替阴影与描边——既克制又不发光。
 *   · 层级：小号大写字母 + 字距（cap）作标签；大号数字作主角；单位弱化。
 *   · 动效：数值 320ms ease-out 补间；切页 OVER 滑动 260ms；页签下划线指示。
 *   · 性能：全程无 shadow / 无渐变 / 无实心大色块，绘制成本低于旧版。
 *
 * 页面：HOME（时间 + 三项指标）/ MOTION（运动曲线 + 数值）/ SYSTEM（内存 + 系统）
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
#include <string.h>

static const char *TAG = "UI/UI";

/* ---------------- 调色板（唯一事实源） ---------------- */
#define C_BG        lv_color_hex(0x0B0E13)      /* 底：近黑带一点冷 */
#define C_LINE      lv_color_hex(0x1E2530)      /* 细线 / 轨道 */
#define C_TEXT      lv_color_hex(0xE9EEF5)      /* 主文字：近白 */
#define C_SUB       lv_color_hex(0x8C99AB)      /* 次文字 */
#define C_FAINT     lv_color_hex(0x59657A)      /* 标签 / 单位 */
#define C_ACC       lv_color_hex(0xD9B98C)      /* 主强调：香槟金 */
#define C_ACC2      lv_color_hex(0x6FA8FF)      /* 辅助：冷蓝（仅曲线） */
#define C_OK        lv_color_hex(0x7BE0B0)      /* 正常 */
#define C_WARN      lv_color_hex(0xE8A86B)      /* 警告 */
#define C_ERR       lv_color_hex(0xE8738C)      /* 错误 */

#define PAGE_COUNT  3
#define TAB_H       32
#define TAB_Y       (BSP_LCD_HEIGHT - TAB_H)
#define PAD         16
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
static lv_obj_t *s_val_light;
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

/* 前向声明 */
static void nav_timer_cb(lv_timer_t *t);

/* ================================================================
 *                        基础绘制件
 * ================================================================ */

/** 透明容器（只做布局锚点/点击热区，不参与视觉） */
static lv_obj_t *mk_box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    return o;
}

/** 1px 细线：替代阴影与描边，克制且不发光 */
static lv_obj_t *mk_hairline(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w)
{
    lv_obj_t *o = mk_box(parent, x, y, w, 1);
    lv_obj_set_style_bg_color(o, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    return o;
}

/** 文本标签 */
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

/** 小号大写标签 + 字距（"精致感"最廉价也最有效的来源） */
static lv_obj_t *mk_cap(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, const char *txt)
{
    lv_obj_t *l = mk_text(parent, x, y, &lv_font_montserrat_12, C_FAINT, txt);
    lv_obj_set_style_text_letter_space(l, 2, LV_PART_MAIN);
    return l;
}

/** 6px 状态圆点 */
static lv_obj_t *mk_dot(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_color_t color)
{
    lv_obj_t *o = mk_box(parent, x, y, 6, 6);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    return o;
}

/** 4px 细轨道（无夸张圆角） */
static lv_obj_t *mk_bar(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_color_t color)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_size(b, w, 4);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, y);
    lv_bar_set_range(b, 0, 100);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, color, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 2, LV_PART_INDICATOR);
    return b;
}

/* ---------------- 数值补间：让数字"滑"到新值而不是跳变 ---------------- */
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

static void num_tween(lv_obj_t *label, int32_t from, int32_t to,
                      uint8_t decimals, const char *suffix, uint32_t ms)
{
    static num_tween_t s_slots[4];
    static uint8_t s_slot = 0;

    num_tween_t *t = &s_slots[s_slot];
    s_slot = (uint8_t)((s_slot + 1U) % 4U);

    t->label    = label;
    t->suffix   = suffix;
    t->decimals = decimals;

    num_tween_apply(t, to);          /* 先落终值：动画只做视觉修饰 */

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, num_tween_cb);
    lv_anim_start(&a);
}

/* ================================================================
 *                        顶部 / 底部常驻区
 * ================================================================ */
static void chrome_build(lv_obj_t *scr)
{
    mk_dot(scr, PAD, 15, C_ACC);                          /* 一点强调色，克制 */
    lv_obj_t *brand = mk_text(scr, PAD + 12, 9, &lv_font_montserrat_12, C_SUB, "AI BOX");
    lv_obj_set_style_text_letter_space(brand, 2, LV_PART_MAIN);

    mk_hairline(scr, 0, TAB_Y, BSP_LCD_WIDTH);            /* 分页区上边线 */
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
        /* 点击热区：整列可点，不需要可见底框 */
        lv_obj_t *hit = mk_box(scr, (lv_coord_t)(i * step), TAB_Y, step, TAB_H);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hit, tab_click_cb, LV_EVENT_CLICKED, (void *)(lv_uintptr_t)i);

        lv_obj_t *l = lv_label_create(scr);
        lv_label_set_text(l, names[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, LV_PART_MAIN);

        lv_obj_set_style_text_color(l, (i == self) ? C_TEXT : C_FAINT, LV_PART_MAIN);
        lv_obj_align(l, LV_ALIGN_TOP_MID,
                     (lv_coord_t)(centers[i] - BSP_LCD_WIDTH / 2), TAB_Y + 8);

        /* 活动项下划线（2px） */
        lv_obj_t *u = mk_box(scr, (lv_coord_t)(centers[i] - 14), TAB_Y + 23, 28, 2);
        lv_obj_set_style_radius(u, 1, LV_PART_MAIN);
        if (i == self) {
            lv_obj_set_style_bg_color(u, C_ACC, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(u, LV_OPA_COVER, LV_PART_MAIN);
        }

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

    /* 主角：时间（大字号 + 微字距） */
    s_lbl_time = mk_text(scr, PAD, 34, &lv_font_montserrat_40, C_TEXT, "--:--");

    s_lbl_date = mk_text(scr, PAD + 2, 82, &lv_font_montserrat_12, C_FAINT, "---- - -- - --");

    mk_hairline(scr, PAD, 110, CONTENT_W);

    /* 三项指标：CAP 标签 / 大数值 / 弱化单位 */
    mk_cap(scr, PAD, 124, "TEMP");
    s_val_temp = mk_text(scr, PAD, 140, &lv_font_montserrat_28, C_ACC, "--.-");
    mk_text(scr, PAD + 70, 152, &lv_font_montserrat_12, C_FAINT, "C");

    mk_cap(scr, PAD + 104, 124, "LUX");
    s_val_light = mk_text(scr, PAD + 104, 140, &lv_font_montserrat_28, C_TEXT, "----");

    mk_cap(scr, PAD + 208, 124, "MOTION");
    s_dot_motion = mk_dot(scr, PAD + 208, 145, C_OK);
    s_val_motion = mk_text(scr, PAD + 208, 157, &lv_font_montserrat_14, C_SUB, "steady");

    tabs_build(scr, 0);
}

/* ================================================================
 *                        第 2 页：MOTION
 * ================================================================ */
static void page_motion_build(void)
{
    lv_obj_t *scr = s_scr[1];

    chrome_build(scr);
    mk_cap(scr, PAD, 32, "MOTION VECTOR");

    s_chart = lv_chart_create(scr);
    lv_obj_set_size(s_chart, CONTENT_W, 116);
    lv_obj_align(s_chart, LV_ALIGN_TOP_LEFT, PAD, 50);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, 64);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, -200, 200);
    lv_chart_set_div_line_count(s_chart, 4, 8);
    /* 透明底 + 极淡网格：曲线浮在底色上，最干净的画法 */
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_chart, C_LINE, LV_PART_MAIN);      /* 网格线 */
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);           /* 曲线粗细 */
    lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR);             /* 不画数据点 */

    s_ser[0] = lv_chart_add_series(s_chart, C_ACC2, LV_CHART_AXIS_PRIMARY_Y);
    s_ser[1] = lv_chart_add_series(s_chart, C_OK,   LV_CHART_AXIS_PRIMARY_Y);
    s_ser[2] = lv_chart_add_series(s_chart, C_ACC,  LV_CHART_AXIS_PRIMARY_Y);

    mk_hairline(scr, PAD, 172, CONTENT_W);

    mk_cap(scr, PAD, 180, "ACC G");
    s_val_acc = mk_text(scr, PAD, 194, &lv_font_montserrat_14, C_SUB, "--  --  --");

    mk_cap(scr, PAD + 168, 180, "GYRO DPS");
    s_val_gyro = mk_text(scr, PAD + 168, 194, &lv_font_montserrat_14, C_SUB, "--  --  --");

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
    mk_cap(scr, PAD, 32, "MEMORY");

    /* INTERNAL RAM：左标签 / 右对齐数值 / 下面细轨道 */
    mk_text(scr, PAD, 50, &lv_font_montserrat_12, C_SUB, "Internal");
    s_val_ram = mk_text(scr, val_x, 50, &lv_font_montserrat_12, C_FAINT, "--");
    lv_obj_set_style_width(s_val_ram, 150, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_val_ram, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    s_bar_ram = mk_bar(scr, PAD, 68, CONTENT_W, C_ACC);

    /* PSRAM */
    mk_text(scr, PAD, 84, &lv_font_montserrat_12, C_SUB, "PSRAM");
    s_val_ps = mk_text(scr, val_x, 84, &lv_font_montserrat_12, C_FAINT, "--");
    lv_obj_set_style_width(s_val_ps, 150, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_val_ps, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    s_bar_ps = mk_bar(scr, PAD, 102, CONTENT_W, C_ACC2);

    mk_hairline(scr, PAD, 120, CONTENT_W);

    mk_cap(scr, PAD, 128, "SYSTEM");
    s_val_fps = mk_text(scr, PAD, 144, &lv_font_montserrat_14, C_SUB, "fps --");
    s_val_mod = mk_text(scr, PAD, 166, &lv_font_montserrat_12, C_FAINT, "--");
    lv_obj_set_style_width(s_val_mod, CONTENT_W, LV_PART_MAIN);
    lv_label_set_long_mode(s_val_mod, LV_LABEL_LONG_WRAP);

    tabs_build(scr, 2);
}

/* ================================================================
 *                        数据刷新
 * ================================================================ */
static void home_timer_cb(lv_timer_t *t)
{
    BSP_UNUSED(t);

    bsp_datetime_t dt;
    if (bsp_rtc_get(&dt) == ESP_OK) {
        lv_label_set_text_fmt(s_lbl_time, "%02u:%02u", dt.hour, dt.minute);
        lv_label_set_text_fmt(s_lbl_date, "%04u - %02u - %02u", dt.year, dt.month, dt.day);
    }

    float c = 0.0f;
    if (bsp_temperature_read(&c) == ESP_OK) {
        int32_t tenths = (int32_t)(c * 10.0f);
        if (tenths < 0) {
            tenths = 0;
        } else if (tenths > 900) {
            tenths = 900;
        }
        num_tween(s_val_temp, 0, tenths, 1, "", 320);
    }

    uint16_t als = 0;
    if (bsp_light_read_als(&als) == ESP_OK) {
        num_tween(s_val_light, 0, (int32_t)(als > 9999U ? 9999U : als), 0, "", 320);
    }

    bsp_imu_data_t imu;
    bool ok = (bsp_imu_read(&imu) == ESP_OK);
    float mag2 = ok ? (imu.acc_g.x * imu.acc_g.x + imu.acc_g.y * imu.acc_g.y +
                       imu.acc_g.z * imu.acc_g.z) : 0.0f;
    bool moving = ok && ((mag2 > 1.35f) || (mag2 < 0.65f));
    lv_obj_set_style_bg_color(s_dot_motion, ok ? (moving ? C_WARN : C_OK) : C_ERR, LV_PART_MAIN);
    lv_label_set_text(s_val_motion, !ok ? "n/a" : (moving ? "moving" : "steady"));
    lv_obj_set_style_text_color(s_val_motion, ok ? C_SUB : C_ERR, LV_PART_MAIN);
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
                lv_obj_set_style_bg_color(s_tab_und[p][i], active ? C_ACC : C_BG, LV_PART_MAIN);
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

    ESP_LOGI(TAG, "dashboard built: %d pages (elegant dark)", PAGE_COUNT);
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
                     260, 0, false);
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
