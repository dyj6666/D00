/* ================================================================
 * gui_app —— GUI 应用层（LVGL v8.3.5 页面框架）
 *
 * 界面架构（240x320 深色主题，三页面 + 底部导航）：
 *   - GuiPages_Init 构建 主页仪表盘 / 网络监控 / 系统监控 三屏
 *   - 底部导航栏（HOME/NET/SYS）带切换动画，页面对象常驻零重建
 *   - GuiPages_Refresh 每秒采集全部外设数据并刷新控件
 *   - 吞吐曲线 1s 一点滚动（60 点 = 1 分钟窗口）
 * 渲染任务：lv_timer_handler 每 10ms 周期驱动（事件驱动，无忙轮询）；
 * 性能基准（gui bench）在独立临时屏执行，不破坏页面对象。
 * ================================================================ */
#include "gui_app.h"
#include "lvgl.h"
#include "lv_port.h"
#include "lv_port_disp.h"
#include "gui_pages.h"
#include "bsp_lcd.h"
#include "app_config.h"
#include "mem_map.h"

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "logger.h"
#include "event_bus.h"
#include "msg_types.h"
#include "cam_link.h"
#include "buzzer_app.h"
#include "stm32f4xx.h"   /* DWT 周期计数器 */

#include <string.h>

#define GUI_TASK_STACK  (8u * 1024u)   /* 8KB：gui bench 全量渲染（lv_refr_now 深调用）
                                        * 峰值超 4KB（实测骨架 1.5KB，全量渲染更高）；
                                        * 4KB 时 bench 栈溢出踩堆 → 对象指针损坏
                                        * → lv_obj_get_parent HardFault（见日志 13.2） */
#define GUI_PERIOD_MS   10u

#define GUI_LVGL_VERSION_STR  "LVGL v8.3.5"

/* 性能基准请求标志 + 前置声明（实现见文件尾部基准段） */
static volatile uint8_t s_bench_request;
static void gui_bench_run(void);

/* ---------------- 按键 → 页面导航 ----------------
 * KEY0 短按：翻页（HOME→NET→SYS→HOME）；长按：回主页。
 * 事件总线回调运行在 eventBusTask 上下文（优先级高于 GUI 任务），
 * LVGL API 非线程安全，回调只置标志，实际切换由 GUI 任务消费执行。 */
static volatile uint8_t s_key_page_next;
static volatile uint8_t s_key_home;

static void gui_on_key_event(const message_t *msg)
{
    if (msg == NULL) {
        return;
    }
    if (msg->hdr.type == MSG_KEY_SHORT) {
        s_key_page_next = 1;
    } else if (msg->hdr.type == MSG_KEY_LONG) {
        s_key_home = 1;
    }
}

/* ---------------- 渲染任务：LVGL 周期驱动 + 250ms 三相轮转刷新 ---------------- */
static void gui_task(void *arg)
{
    (void)arg;
    LOG_Printf("[GUI] task enter\r\n");
    uint32_t last = 0;
    for (;;) {
        lv_timer_handler();
        /* 按键导航（事件总线回调置位，本任务上下文串行消费）：
         * 短按翻页、长按回主页——与触摸导航栏等效 */
        if (s_key_page_next) {
            s_key_page_next = 0;
            GuiPages_PageNext();
        }
        if (s_key_home) {
            s_key_home = 0;
            GuiPages_ShowHome();
        }
        /* 摄像头挥手翻页（cam_link 服务层事件标志，250ms 节拍内消费）：
         * 挥手 SWIPE_LEFT/RIGHT → 页面轮换 + 蜂鸣提示，与 KEY0 短按等效 */
        uint8_t swipe_dir = 0;
        if (CamLink_ConsumeSwipe(&swipe_dir) && swipe_dir != 0u) {
            GuiPages_PageNext();
            Buzzer_Beep(30);   /* 换页提示音 */
        }
        /* 性能基准请求（命令上下文置位，本任务上下文串行执行） */
        if (s_bench_request) {
            s_bench_request = 0;
            gui_bench_run();
        }
        /* 250ms 三相轮转刷新（采集/曲线/文本分片，彻底错峰——避免所有
         * 控件同帧爆发重绘；数据粒度 250ms 使数值更新更平滑） */
        if (lv_tick_get() - last >= 250u) {
            last = lv_tick_get();
            GuiPages_RefreshFast();
        }
        vTaskDelay(pdMS_TO_TICKS(GUI_PERIOD_MS));
    }
}

/* ================================================================
 * 性能基准（`gui bench` 触发，GUI 任务上下文串行执行）
 * 基准场景运行在独立临时屏（s_bench_scr），结束恢复主页，
 * 保证基准期间的 clean/重建不破坏常驻页面对象。
 * ================================================================ */
#define BENCH_CYC_TO_US(c)   ((uint32_t)((c) / 168u))
#define BENCH_MEM_WORDS      (16u * 1024u)   /* 64KB 块 */

static lv_obj_t *s_bench_scr;   /* 基准专用临时屏 */

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
        /* UI 场景：加载真实主页（最接近实际负载的基准对象）；
         * 结束后立即切回独立临时屏——后续场景的 clean 绝不能落在主页上
         * （会清空页面对象，悬垂指针导致 GuiPages_Refresh 崩溃，
         * 见 ENGINEERING_LOG 13.2） */
        lv_obj_t *home = GuiPages_GetHome();
        if (home != NULL) {
            lv_scr_load(home);
            lv_obj_invalidate(home);
        }
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
    if (need_ui) {
        lv_scr_load(s_bench_scr);   /* 关键：恢复独立临时屏，防后续 clean 破坏主页 */
    }
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
    /* 先切回独立临时屏再 clean：ui 场景可能已把活动屏切到主页，
     * 直接 clean(活动屏) 会清空主页对象（悬垂指针崩溃，见 13.2） */
    lv_scr_load(s_bench_scr);
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
    lv_obj_t *home = GuiPages_GetHome();
    if (home == NULL) {
        return;
    }
    lv_scr_load(home);
    if (!shadow) {
        /* 递归关掉全部子对象的阴影 */
        lv_obj_t *c;
        uint32_t idx = 0;
        while ((c = lv_obj_get_child(home, idx)) != NULL) {
            lv_obj_set_style_shadow_width(c, 0, LV_PART_MAIN);
            idx++;
        }
    }
    lv_scr_load(s_bench_scr);   /* 恢复独立临时屏（防后续 clean 破坏主页） */
}

/* ---- 基准总入口（GUI 任务上下文；运行在独立临时屏） ---- */
static void gui_bench_run(void)
{
    LOG_Printf("===== GUI BENCH (168MHz, " GUI_LVGL_VERSION_STR ", %ux%u) =====\r\n",
               (unsigned)BSP_LCD_GetWidth(), (unsigned)BSP_LCD_GetHeight());

    /* 切到独立临时屏：基准场景的 clean/重建不影响常驻页面 */
    if (s_bench_scr == NULL) {
        s_bench_scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_bench_scr, lv_color_hex(0x0E1420), 0);
    }
    lv_scr_load(s_bench_scr);

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
    LOG_Printf("===== GUI BENCH DONE =====\r\n");

    /* 恢复主页显示（页面对象常驻未被破坏） */
    GuiPages_ShowHome();
}

/* ---------------- 初始化（模块注册入口） ---------------- */
void GuiApp_Init(void)
{
    uint16_t id = BSP_LCD_Init();   /* LCD 硬件初始化（BSP 层） */
    LOG_Printf("[GUI] LVGL  : lcd id=0x%04X, %ux%u, " GUI_LVGL_VERSION_STR "\r\n",
               id, BSP_LCD_GetWidth(), BSP_LCD_GetHeight());

    lv_init();          /* LVGL 核心（堆位于外部 SRAM） */
    LvPort_Init();      /* 显示 + 触摸端口 */
    GuiPages_Init();    /* 构建三页面并加载主页 */

    /* 按键 → 页面导航：短按翻页 / 长按回主页（LED/蜂鸣器已有各自订阅，
     * 事件总线为广播语义，多订阅互不冲突） */
    EventBus_Subscribe(MSG_KEY_SHORT, gui_on_key_event);
    EventBus_Subscribe(MSG_KEY_LONG, gui_on_key_event);

    osThreadAttr_t attr = {
        .name       = "GuiApp",
        .stack_size = GUI_TASK_STACK,
        /* 优先级保持 Normal(24)：曾试提至 32 与 TouchSvc/tcpip 同级，
         * 同级时间片轮转导致渲染被触摸采样任务每 10ms 打断、碎片化，
         * 实测触摸"卡到不起作用"（见 ENGINEERING_LOG 13.3）。24 时
         * 触摸采样任务(32)可抢占 GUI，但每次仅几十 µs，可接受。 */
        .priority   = osPriorityNormal,
    };
    osThreadId_t h = osThreadNew(gui_task, NULL, &attr);
    LOG_Printf("[GUI] task %s (stack=%u, heap=%u)\r\n",
               (h != NULL) ? "started" : "FAILED",
               (unsigned)GUI_TASK_STACK,
               (unsigned)xPortGetFreeHeapSize());
}
