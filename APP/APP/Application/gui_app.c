/* ================================================================
 * gui_app —— GUI 应用层实现（LVGL v8.3.5 骨架）
 *
 * 界面设计（240x320 深色主题，验证全链路）：
 *   - 标题区：D00 GUI + LVGL 版本徽标
 *   - 信息卡片：固件版本（读 RUN 尾部）+ 分区方案B 标识
 *   - 交互区：按钮（计数）+ 滑块（数值联动）——验证触摸输入
 *   - 状态区：系统 tick 时钟 + 触摸坐标实时显示
 * 渲染任务：lv_timer_handler 每 10ms 周期驱动（事件驱动，无忙轮询）。
 * ================================================================ */
#include "gui_app.h"
#include "lvgl.h"
#include "lv_port.h"
#include "bsp_lcd.h"
#include "app_config.h"
#include "touch_svc.h"

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "logger.h"

#include <stdio.h>

#define GUI_TASK_STACK  (4u * 1024u)   /* osThreadNew stack_size 单位：字节（4KB，峰值实测 1.5KB） */
#define GUI_PERIOD_MS   10u

/* 外部 SRAM 布局（方案B，见 lv_port_disp.c）：LVGL 堆 0x68080000 起 */
#define GUI_LVGL_VERSION_STR  "LVGL v8.3.5"

static lv_obj_t *s_count_label;      /* 按钮计数显示 */
static lv_obj_t *s_slider_value;     /* 滑块数值显示 */
static lv_obj_t *s_clock_label;      /* 系统时钟显示 */
static lv_obj_t *s_touch_label;      /* 触摸坐标显示 */
static uint32_t s_count;

/* ---------------- 事件回调（触摸/交互验证） ---------------- */
static void gui_btn_event(lv_event_t *e)
{
    (void)e;
    s_count++;
    lv_label_set_text_fmt(s_count_label, "Count: %lu", (unsigned long)s_count);
}

static void gui_slider_event(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_label_set_text_fmt(s_slider_value, "%d%%",
                          (int)lv_slider_get_value(slider));
}

/* ---------------- 界面构建（骨架：标题/卡片/交互/状态） ---------------- */
static void gui_build(void)
{
    uint32_t ver = *(volatile uint32_t *)OTA_APP_VERSION_ADDR;

    /* 最小渲染测试：纯色背景 + 单 label（避开按钮/滑块/主题渐变） */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1420), 0);
    lv_obj_t *t = lv_label_create(scr);
    /* 标题 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "D00  GUI");
    lv_obj_set_style_text_color(title, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, GUI_LVGL_VERSION_STR "  |  Plan-B 832KB");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x90A4AE), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 42);

    /* 固件信息卡片 */
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 216, 62);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A2230), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2E3A4E), 0);
    lv_obj_set_style_radius(card, 8, 0);

    lv_obj_t *ver_l = lv_label_create(card);
    lv_label_set_text_fmt(ver_l, "Firmware  v%lu", (unsigned long)ver);
    lv_obj_set_style_text_color(ver_l, lv_color_hex(0xECEFF1), 0);
    lv_obj_align(ver_l, LV_ALIGN_TOP_LEFT, 12, 8);

    lv_obj_t *rt_l = lv_label_create(card);
    lv_label_set_text(rt_l, "LVGL core linked");
    lv_obj_set_style_text_color(rt_l, lv_color_hex(0x66BB6A), 0);
    lv_obj_align(rt_l, LV_ALIGN_TOP_LEFT, 12, 32);

    /* 交互区：按钮 + 滑块 */
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 100, 40);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, -52, 150);
    lv_obj_add_event_cb(btn, gui_btn_event, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E88E5), 0);

    lv_obj_t *btn_l = lv_label_create(btn);
    lv_label_set_text(btn_l, "Tap Me");
    lv_obj_center(btn_l);

    s_count_label = lv_label_create(scr);
    lv_label_set_text(s_count_label, "Count: 0");
    lv_obj_set_style_text_color(s_count_label, lv_color_hex(0xFFD54F), 0);
    lv_obj_align(s_count_label, LV_ALIGN_TOP_LEFT, 24, 158);

    lv_obj_t *slider = lv_slider_create(scr);
    lv_obj_set_size(slider, 120, 16);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, -50, 210);
    lv_obj_add_event_cb(slider, gui_slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 60, LV_ANIM_ON);

    s_slider_value = lv_label_create(scr);
    lv_label_set_text(s_slider_value, "60%");
    lv_obj_set_style_text_color(s_slider_value, lv_color_hex(0x4DB6AC), 0);
    lv_obj_align(s_slider_value, LV_ALIGN_TOP_RIGHT, -18, 208);

    /* 状态区：时钟 + 触摸 */
    s_clock_label = lv_label_create(scr);
    lv_label_set_text(s_clock_label, "t=0 ms");
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(0x90A4AE), 0);
    lv_obj_align(s_clock_label, LV_ALIGN_BOTTOM_LEFT, 12, -8);

    s_touch_label = lv_label_create(scr);
    lv_label_set_text(s_touch_label, "touch: --,--");
    lv_obj_set_style_text_color(s_touch_label, lv_color_hex(0x90A4AE), 0);
    lv_obj_align(s_touch_label, LV_ALIGN_BOTTOM_RIGHT, -12, -8);
}

/* ---------------- 状态刷新（1s 节拍，仅更新文本） ---------------- */
static void gui_refresh(void)
{
    lv_label_set_text_fmt(s_clock_label, "t=%lu ms",
                          (unsigned long)lv_tick_get());

    const touch_svc_state_t *ts = TouchSvc_GetState();
    if (ts->state == TOUCH_EVT_DOWN || ts->state == TOUCH_EVT_MOVE) {
        lv_label_set_text_fmt(s_touch_label, "touch: %u,%u",
                              (unsigned)ts->x, (unsigned)ts->y);
    } else {
        lv_label_set_text(s_touch_label, "touch: --,--");
    }
}

/* ---------------- 渲染任务：LVGL 周期驱动 ---------------- */
static void gui_task(void *arg)
{
    (void)arg;
    LOG_Printf("[GUI] task enter\r\n");
    uint32_t last = 0;
    for (;;) {
        lv_timer_handler();
        /* 1s 状态节拍 */
        if (lv_tick_get() - last >= 1000u) {
            last = lv_tick_get();
            gui_refresh();
        }
        vTaskDelay(pdMS_TO_TICKS(GUI_PERIOD_MS));
    }
}

/* ---------------- 初始化（模块注册入口） ---------------- */
void GuiApp_Init(void)
{
    uint16_t id = BSP_LCD_Init();   /* LCD 硬件初始化（BSP 层） */
    LOG_Printf("[GUI] LVGL  : lcd id=0x%04X, %ux%u, " GUI_LVGL_VERSION_STR "\r\n",
               id, BSP_LCD_GetWidth(), BSP_LCD_GetHeight());

    lv_init();          /* LVGL 核心（堆位于外部 SRAM） */
    LvPort_Init();      /* 显示 + 触摸端口 */
    gui_build();        /* 骨架界面 */

    osThreadAttr_t attr = {
        .name       = "GuiApp",
        .stack_size = GUI_TASK_STACK,
        .priority   = osPriorityNormal,
    };
    osThreadId_t h = osThreadNew(gui_task, NULL, &attr);
    LOG_Printf("[GUI] task %s (stack=%u, heap=%u)\r\n",
               (h != NULL) ? "started" : "FAILED",
               (unsigned)GUI_TASK_STACK,
               (unsigned)xPortGetFreeHeapSize());
}
