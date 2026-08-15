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
#include "lv_port_disp.h"
#include "bsp_lcd.h"
#include "app_config.h"
#include "mem_map.h"
#include "touch_svc.h"

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "logger.h"
#include "stm32f4xx.h"   /* DWT 周期计数器 */

#include <stdio.h>
#include <string.h>

#define GUI_TASK_STACK  (4u * 1024u)   /* osThreadNew stack_size 单位：字节（4KB，峰值实测 1.5KB） */
#define GUI_PERIOD_MS   10u

/* 外部 SRAM 布局（方案B，见 lv_port_disp.c）：LVGL 堆 0x68080000 起 */
#define GUI_LVGL_VERSION_STR  "LVGL v8.3.5"

static lv_obj_t *s_count_label;      /* 按钮计数显示 */
static lv_obj_t *s_slider_value;     /* 滑块数值显示 */
static lv_obj_t *s_clock_label;      /* 系统时钟显示 */
static lv_obj_t *s_touch_label;      /* 触摸坐标显示 */
static uint32_t s_count;

/* 性能基准请求标志 + 前置声明（实现见文件尾部基准段） */
static volatile uint8_t s_bench_request;
static void gui_bench_run(void);

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
        /* 性能基准请求（命令上下文置位，本任务上下文串行执行） */
        if (s_bench_request) {
            s_bench_request = 0;
            gui_bench_run();
            gui_build();   /* 基准结束后恢复骨架界面 */
        }
        /* 1s 状态节拍 */
        if (lv_tick_get() - last >= 1000u) {
            last = lv_tick_get();
            gui_refresh();
        }
        vTaskDelay(pdMS_TO_TICKS(GUI_PERIOD_MS));
    }
}

/* ================================================================
 * 性能基准（`gui bench` 触发，GUI 任务上下文串行执行）
 * ================================================================ */
#define BENCH_CYC_TO_US(c)   ((uint32_t)((c) / 168u))
#define BENCH_MEM_WORDS      (16u * 1024u)   /* 64KB 块 */

void GuiApp_Bench(void)
{
    s_bench_request = 1;
}

/* ---- 内存读带宽（MB/s @168MHz，不破坏目标区内容） ---- */
static uint32_t bench_mem_rd(const uint32_t *src, uint32_t words)
{
    volatile uint32_t sink = 0;
    uint32_t t0 = DWT->CYCCNT;
    for (uint32_t i = 0; i < words; i++) {
        sink ^= src[i];
    }
    uint32_t cyc = DWT->CYCCNT - t0;
    (void)sink;
    return (uint32_t)((uint64_t)words * 4u * 168000000u / cyc / 1000000u);
}

/* ---- 外部 SRAM 写带宽（自有空闲区，安全） ---- */
static uint32_t bench_mem_wr(uint32_t *dst, uint32_t words)
{
    uint32_t t0 = DWT->CYCCNT;
    for (uint32_t i = 0; i < words; i++) {
        dst[i] = 0x55AA1234u ^ i;
    }
    uint32_t cyc = DWT->CYCCNT - t0;
    return (uint32_t)((uint64_t)words * 4u * 168000000u / cyc / 1000000u);
}

static void bench_mem(void)
{
    /* 外部 SRAM 空闲区（mem_map.h：预触发缓冲之后到 1MB 末尾，~340KB 空闲） */
    uint32_t *ext = (uint32_t *)(MEM_LA_PRETRIG_BASE + MEM_LA_PRETRIG_SIZE + 0x100u);
    ext = (uint32_t *)(((uint32_t)ext + 31u) & ~31u);   /* 32B 对齐 */

    LOG_Printf("[mem] EXT-SRAM  rd=%lu MB/s  wr=%lu MB/s\r\n",
               (unsigned long)bench_mem_rd(ext, BENCH_MEM_WORDS),
               (unsigned long)bench_mem_wr(ext, BENCH_MEM_WORDS));
    LOG_Printf("[mem] SRAM128   rd=%lu MB/s\r\n",
               (unsigned long)bench_mem_rd((const uint32_t *)0x20000000u, BENCH_MEM_WORDS));
    LOG_Printf("[mem] CCM       rd=%lu MB/s\r\n",
               (unsigned long)bench_mem_rd((const uint32_t *)0x10000000u, BENCH_MEM_WORDS));
    LOG_Printf("[mem] FLASH     rd=%lu MB/s\r\n",
               (unsigned long)bench_mem_rd((const uint32_t *)0x08010000u, BENCH_MEM_WORDS));
}

/* ---- LVGL 场景：强制全屏无效化 + lv_refr_now 同步整帧渲染，
 * 直测单帧渲染+传输耗时（不受 REFR_PERIOD 节流影响，反映极限帧率） ---- */
static void bench_lvgl_scene(const char *name, uint32_t frames,
                             bool force_invalid, bool need_ui)
{
    if (need_ui) {
        lv_obj_clean(lv_scr_act());
        gui_build();
    }
    lv_disp_t *d = lv_disp_get_default();
    LvPort_FlushStatsReset();
    uint64_t sum_cyc = 0;
    for (uint32_t i = 0; i < frames; i++) {
        if (force_invalid) {
            lv_obj_invalidate(lv_scr_act());
        }
        uint32_t t0 = DWT->CYCCNT;
        lv_refr_now(d);
        sum_cyc += (uint64_t)(DWT->CYCCNT - t0);
    }
    lv_flush_stats_t st;
    LvPort_FlushStatsGet(&st);
    uint32_t avg_us = (uint32_t)(sum_cyc / frames / 168u);
    uint32_t fps = (avg_us > 0u) ? (1000000u / avg_us) : 0u;
    /* MPix/s = pixels / (cycles @168MHz) × 168（uint64 中间量防溢出） */
    uint32_t mpx = (st.cycles > 0u)
        ? (uint32_t)((st.pixels * 168u) / st.cycles) : 0u;
    LOG_Printf("[lvgl] %-16s %lu.%02lu ms/frame | %3lu fps | flush %lu MPix/s | %lu px/flush\r\n",
               name,
               (unsigned long)(avg_us / 1000u), (unsigned long)((avg_us % 1000u) / 10u),
               (unsigned long)fps,
               (unsigned long)mpx,
               (unsigned long)(st.calls > 0 ? st.pixels / st.calls : 0u));
}

/* ---- 色带场景：12 条全宽色带覆盖全屏（纯填充压力） ---- */
static void bench_scene_bands(void)
{
    lv_obj_clean(lv_scr_act());
    const uint32_t colors[12] = {
        0xF44336, 0xE91E63, 0x9C27B0, 0x673AB7, 0x3F51B5, 0x2196F3,
        0x03A9F4, 0x00BCD4, 0x009688, 0x4CAF50, 0xCDDC39, 0xFF9800,
    };
    for (uint32_t i = 0; i < 12; i++) {
        lv_obj_t *b = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(b);
        lv_obj_set_style_bg_color(b, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_pos(b, 0, (lv_coord_t)(i * (320 / 12)));
        lv_obj_set_size(b, 240, (320 / 12) + 1);
    }
}

/* ---- 动画场景：60x60 圆角块往返滑动（动画/局部重绘压力） ---- */
static void bench_scene_anim(void)
{
    lv_obj_clean(lv_scr_act());
    lv_obj_t *m = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(m);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_radius(m, 10, 0);
    lv_obj_set_size(m, 60, 60);
    lv_obj_set_pos(m, 0, 0);
}

/* ---- 动画帧测量：手动移动对象 N 步 + lv_refr_now 逐帧统计 ---- */
static void bench_anim_run(void)
{
    lv_obj_t *m = lv_obj_get_child(lv_scr_act(), 0);
    if (m == NULL) {
        return;
    }
    LvPort_FlushStatsReset();
    uint64_t sum_cyc = 0;
    const uint32_t steps = 60;
    for (uint32_t i = 0; i < steps; i++) {
        lv_obj_set_pos(m, (lv_coord_t)((i % 2) ? 180 : 0),
                       (lv_coord_t)(i * 4));
        uint32_t t0 = DWT->CYCCNT;
        lv_refr_now(lv_disp_get_default());
        sum_cyc += (uint64_t)(DWT->CYCCNT - t0);
    }
    lv_flush_stats_t st;
    LvPort_FlushStatsGet(&st);
    uint32_t avg_us = (uint32_t)(sum_cyc / steps / 168u);
    LOG_Printf("[lvgl] anim-60x60       %lu.%02lu ms/frame | %3lu fps | %lu px/frame\r\n",
               (unsigned long)(avg_us / 1000u), (unsigned long)((avg_us % 1000u) / 10u),
               (unsigned long)(avg_us > 0 ? 1000000u / avg_us : 0u),
               (unsigned long)(st.calls > 0 ? st.pixels / st.calls : 0u));
}

/* ---- 纯填充场景：单个全屏 obj（无叠加/无阴影，测渲染下限） ---- */
static lv_obj_t *s_fill_obj;

static void bench_scene_fill(void)
{
    lv_obj_clean(lv_scr_act());
    s_fill_obj = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_fill_obj);
    lv_obj_set_style_bg_color(s_fill_obj, lv_color_hex(0x1A237E), 0);
    lv_obj_set_style_bg_opa(s_fill_obj, LV_OPA_COVER, 0);
    lv_obj_set_size(s_fill_obj, 240, 320);
    lv_obj_set_pos(s_fill_obj, 0, 0);
}

/* ---- 纯填充帧测：每帧换色 invalidate 全屏 ---- */
static void bench_fill_run(void)
{
    lv_disp_t *d = lv_disp_get_default();
    LvPort_FlushStatsReset();
    uint64_t sum_cyc = 0;
    const uint32_t frames = 10;
    for (uint32_t i = 0; i < frames; i++) {
        lv_obj_set_style_bg_color(s_fill_obj,
            lv_color_hex(0x1A237E + i * 0x101010u), 0);
        uint32_t t0 = DWT->CYCCNT;
        lv_refr_now(d);
        sum_cyc += (uint64_t)(DWT->CYCCNT - t0);
    }
    lv_flush_stats_t st;
    LvPort_FlushStatsGet(&st);
    uint32_t avg_us = (uint32_t)(sum_cyc / frames / 168u);
    uint32_t mpx = (st.cycles > 0u)
        ? (uint32_t)((st.pixels * 168u) / st.cycles) : 0u;
    LOG_Printf("[lvgl] fill-fullscreen   %lu.%02lu ms/frame | %3lu fps | flush %lu MPix/s\r\n",
               (unsigned long)(avg_us / 1000u), (unsigned long)((avg_us % 1000u) / 10u),
               (unsigned long)(avg_us > 0 ? 1000000u / avg_us : 0u),
               (unsigned long)mpx);
}

/* ---- UI 场景（可开关阴影对比：阴影为 blur mask 运算大头） ---- */
static void bench_scene_ui(bool shadow)
{
    lv_obj_clean(lv_scr_act());
    gui_build();
    if (!shadow) {
        /* 递归关掉全部子对象的阴影 */
        lv_obj_t *c;
        uint32_t idx = 0;
        while ((c = lv_obj_get_child(lv_scr_act(), idx)) != NULL) {
            lv_obj_set_style_shadow_width(c, 0, LV_PART_MAIN);
            idx++;
        }
    }
}

/* ---- 基准总入口（GUI 任务上下文） ---- */
static void gui_bench_run(void)
{
    LOG_Printf("===== GUI BENCH (168MHz, " GUI_LVGL_VERSION_STR ", %ux%u) =====\r\n",
               (unsigned)BSP_LCD_GetWidth(), (unsigned)BSP_LCD_GetHeight());

    bench_mem();

    LOG_Printf("[lcd] raw layer bench:\r\n");
    BSP_LCD_Bench();

    /* LVGL 场景（lv_refr_now 直测整帧耗时） */
    bench_scene_fill();
    bench_fill_run();
    bench_scene_bands();
    bench_lvgl_scene("bands-fullscreen", 10u, true, false);
    bench_lvgl_scene("ui-shadow-on",     10u, true, true);
    bench_scene_ui(false);
    bench_lvgl_scene("ui-shadow-off",    10u, true, false);
    bench_scene_anim();
    bench_anim_run();
    lv_obj_clean(lv_scr_act());
    LOG_Printf("===== GUI BENCH DONE =====\r\n");
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
