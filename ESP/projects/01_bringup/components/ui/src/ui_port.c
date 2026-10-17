/* ================================================================
 * ui_port.c —— LVGL 端口层（性能核心）
 *
 * 刷新模型（为什么这样最快）：
 *   面板是 8080 并口，10MHz × 8bit = 10MB/s，全屏 150KB 需要 15.4ms
 *   → 理论上限约 65fps。要逼近它，必须让"渲染"与"DMA 搬运"重叠：
 *
 *     ┌─ 渲染块 A ─┐┌─ 渲染块 B ─┐┌─ 渲染块 C ─┐
 *     │  内部 RAM  ││  内部 RAM  ││  内部 RAM  │   ← 双缓冲交替
 *     └─────┬──────┘└─────┬──────┘└─────┬──────┘
 *           │ 异步提交     │ 异步提交     │
 *           ▼             ▼             ▼
 *        [DMA 搬 A]    [DMA 搬 B]    [DMA 搬 C]   ← i80 队列，与渲染并行
 *           │ 完成中断
 *           ▼
 *      lv_disp_flush_ready()  ← 立刻允许 LVGL 复用该缓冲
 *
 *   约束：同一时刻只允许一笔在途（缓冲由 i80 队列持有，不能被改写）。
 *   实现：flush_cb 内若上一笔未完成，等 BSP 的完成信号量——等待发生在
 *         "提交下一块之前"，因此不影响 A 块搬运与 B 块渲染的重叠。
 *
 * 其它性能措施：
 *   - 缓冲放内部 RAM（PSRAM 写带宽只有内部 RAM 的几分之一）；
 *   - LVGL 任务独占 core1，应用逻辑留 core0；
 *   - 时基用 esp_timer（LV_TICK_CUSTOM），没有 1ms 定时器中断开销。
 * ================================================================ */
#include <stdio.h>
#include <string.h>

#include "ui/ui.h"
#include "ui_internal.h"

#include "lvgl.h"

#include "bsp/bsp.h"
#include "bsp/lcd.h"
#include "bsp/touch.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "UI";

/* ---------------- 可调参数 ---------------- */
#define UI_BUF_LINES        60                          /* 每块缓冲行数：320x60 = 38.4KB（内部 RAM，2 块共 77KB） */
#define UI_BUF_PIXELS       (BSP_LCD_WIDTH * UI_BUF_LINES)
#define UI_TASK_STACK       (12 * 1024)
#define UI_TASK_PRIO        5
#define UI_TASK_CORE        1                           /* UI 独占 core1 */
#define UI_DEBUG_SHOT       0    /* 1=上电自动切 FACE 页并回传一张截图（配合 tools/shot.py 做视觉回归） */
#define UI_STARTUP_BENCH    1                           /* 上电跑一次性能基准（回归对比用；量产可关） */

/* ---------------- 显示缓冲（内部 RAM，DMA 可达） ---------------- */
static lv_color_t s_buf0[UI_BUF_PIXELS] __attribute__((aligned(8)));
static lv_color_t s_buf1[UI_BUF_PIXELS] __attribute__((aligned(8)));

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_indev_drv;

static volatile uint32_t s_frame_count = 0;      /* 统计窗口内完成的上屏次数 */
static uint32_t          s_fps         = 0;
static volatile bool     s_running     = false;
static TaskHandle_t      s_task        = NULL;

/* ---------------- 在途交易追踪（允许两笔并飞）----------------
 * i80 队列是先进先出，所以用一个单生产者/单消费者的环形队列配对：
 *   提交时(UI 任务) push 缓冲号；完成时(ISR) pop 队首 → 清对应缓冲的忙标志。
 * 这样"渲染下一块"与"搬运当前块"完全重叠，且不会踩到仍在搬运的缓冲。
 */
#define UI_MAX_INFLIGHT  4
static volatile uint8_t  s_inflight[UI_MAX_INFLIGHT];
static volatile uint32_t s_in_head = 0;
static volatile uint32_t s_in_tail = 0;
static volatile uint8_t  s_buf_busy[2] = { 0, 0 };

/* ---------------- 调试截图：把 flush 的每块拼成整帧（PSRAM） ----------------
 * 本 LVGL 副本没有 snapshot 模块，所以用"抓 DMA 前的那块渲染结果"来合成整帧：
 *   flush_cb 里按行 memcpy 到全屏缓冲 → 触发一次整屏重绘 → 串口回传 → 主机转 PNG。
 * 仅在 ui_debug_dump_screen() 期间生效，平时零开销。
 */
static uint16_t *s_shot;                 /* 320x240 RGB565，PSRAM，按需分配 */
static volatile bool s_shot_active;

static void ui_debug_shot_cb(lv_timer_t *t);

static void shot_capture(const lv_area_t *area, const uint16_t *px)
{
    if (!s_shot_active || (s_shot == NULL)) {
        return;
    }
    const lv_coord_t w = lv_area_get_width(area);
    for (lv_coord_t y = area->y1; y <= area->y2; y++) {
        if (y < 0 || y >= BSP_LCD_HEIGHT) {
            continue;
        }
        memcpy(&s_shot[(size_t)y * BSP_LCD_WIDTH + (size_t)area->x1],
               &px[(size_t)(y - area->y1) * (size_t)w],
               (size_t)w * sizeof(uint16_t));
    }
}

/* ---------------- 耗时归因（每次 flush 内部 / 两次 flush 之间的渲染）---------------- */
static volatile uint64_t s_flush_us_acc  = 0;   /* flush_cb 内部总耗时（含等待/提交） */
static volatile uint64_t s_render_us_acc = 0;   /* 两次 flush 之间的渲染耗时 */
static volatile uint32_t s_flush_calls   = 0;
static volatile uint64_t s_last_exit_us  = 0;

/* ---------------- 日志转发到 ESP_LOG ---------------- */
static void ui_log_cb(const char *buf)
{
    ESP_LOGW("LVGL", "%s", buf);
}

/* ---------------- 传输完成（ISR 上下文） ---------------- */
static void IRAM_ATTR ui_flush_done_isr(void)
{
    if (s_in_head != s_in_tail) {
        s_buf_busy[s_inflight[s_in_head]] = 0;      /* 队首那笔搬完了 */
        s_in_head = (s_in_head + 1U) % UI_MAX_INFLIGHT;
    }
    s_frame_count++;
    /* 立刻放行 LVGL：允许它复用刚搬完的缓冲，渲染流水线不中断 */
    lv_disp_flush_ready(&s_disp_drv);
}

/* ---------------- 显示刷新回调 ---------------- */
static void ui_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px_map)
{
    const uint64_t t_enter = esp_timer_get_time();
    shot_capture(area, (const uint16_t *)px_map);     /* 调试截图：只在抓帧期间生效 */

    if (s_last_exit_us != 0U) {
        s_render_us_acc += t_enter - s_last_exit_us;   /* 上一笔提交完 → 本次渲染结束 */
    }

    const uint8_t idx = (px_map == s_buf1) ? 1U : 0U;

    /* 只有"该缓冲自己的上一笔"还没搬完才需要等。
     * ⚠ 必须等 BSP 的完成信号量（ISR 唤醒，微秒级），不能用 vTaskDelay(1)——
     * 1000Hz tick 下那等于每次硬等 1ms。 */
    while (s_buf_busy[idx]) {
        if (bsp_lcd_wait_flush_done(50) != ESP_OK) {
            break;                          /* 超时兜底：宁可冒险也不死锁 */
        }
    }

    s_buf_busy[idx] = 1U;
    s_inflight[s_in_tail] = idx;
    s_in_tail = (s_in_tail + 1U) % UI_MAX_INFLIGHT;

    esp_err_t err = bsp_lcd_draw_bitmap_async((uint16_t)area->x1, (uint16_t)area->y1,
                                              (uint16_t)(area->x2 + 1), (uint16_t)(area->y2 + 1),
                                              (const uint16_t *)px_map);
    if (err != ESP_OK) {
        /* 提交失败：撤销这次在途登记并放行，避免死锁 */
        if (s_in_head != s_in_tail) {
            s_in_head = (s_in_head + 1U) % UI_MAX_INFLIGHT;
        }
        s_buf_busy[idx] = 0U;
        lv_disp_flush_ready(drv);
    }

    s_flush_us_acc += esp_timer_get_time() - t_enter;
    s_flush_calls++;
    s_last_exit_us = esp_timer_get_time();
}

/* ---------------- 触摸输入回调 ---------------- */
static void ui_input_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    BSP_UNUSED(drv);

    static uint16_t last_x = 0;
    static uint16_t last_y = 0;

    bsp_touch_state_t st;
    if (bsp_touch_read(&st) == ESP_OK && st.pressed && st.count > 0U) {
        last_x = st.point[0].x;
        last_y = st.point[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = last_x;
    data->point.y = last_y;
}

/* ---------------- 帧率统计 ---------------- */
static void ui_fps_timer_cb(lv_timer_t *t)
{
    BSP_UNUSED(t);
    s_fps = s_frame_count;
    s_frame_count = 0;
}

/* ---------------- 显示吞吐自测（上电 3s 后跑一次）----------------
 * 全屏失效 + 立即重绘 N 次，测"渲染 + 上屏"的真实吞吐。
 * 这是"丝滑上限"的硬指标：受 8080 总线带宽约束，
 * 10MHz × 8bit = 10MB/s → 全屏 150KB 理论 15.4ms ≈ 65fps。
 */
static void ui_perf_selftest_cb(lv_timer_t *t)
{
    lv_timer_del(t);

#if UI_STARTUP_BENCH

    /* ---- ① 纯总线吞吐：整屏 150KB 一次 DMA（不经过 LVGL），这是硬上限 ---- */
    const uint16_t *fb = bsp_lcd_fb();
    uint32_t bus_ms = 0;
    if (fb != NULL) {
        const int bn = 3;
        uint32_t b0 = (uint32_t)(esp_timer_get_time() / 1000);
        for (int i = 0; i < bn; i++) {
            (void)bsp_lcd_draw_bitmap(0, 0, BSP_LCD_WIDTH, BSP_LCD_HEIGHT, fb);
        }
        bus_ms = ((uint32_t)(esp_timer_get_time() / 1000) - b0) / (uint32_t)bn;
    }

    /* ---- ② LVGL 全屏重绘：渲染 + 上屏（含双缓冲重叠）+ 耗时归因 ---- */
    s_flush_us_acc = 0;
    s_render_us_acc = 0;
    s_flush_calls = 0;
    s_last_exit_us = 0;

    const int n = 20;
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < n; i++) {
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
    }
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000) - t0;
    uint32_t per = ms / (uint32_t)n;

    const uint32_t flush_ms  = (uint32_t)(s_flush_us_acc / 1000U / (uint64_t)n);
    const uint32_t render_ms = (uint32_t)(s_render_us_acc / 1000U / (uint64_t)n);
    const uint32_t calls     = s_flush_calls / (uint32_t)n;

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);

    ESP_LOGI(TAG, "PERF bus-only full frame : %lu ms -> %lu fps (hard ceiling)",
             (unsigned long)bus_ms, (unsigned long)((bus_ms > 0U) ? (1000U / bus_ms) : 0UL));
    ESP_LOGI(TAG, "PERF lvgl full redraw   : %lu ms -> %lu fps", (unsigned long)per,
             (unsigned long)((per > 0U) ? (1000U / per) : 0UL));
    ESP_LOGI(TAG, "PERF breakdown          : flush_cb %lu ms (%lu calls/frame) + render %lu ms",
             (unsigned long)flush_ms, (unsigned long)calls, (unsigned long)render_ms);
    ESP_LOGI(TAG, "PERF mem: internal heap free=%u B | LVGL heap %u%% used (%u/%u B)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)mon.used_pct,
             (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.total_size);

    /* ---- ③ 极简页面基准：纯色背景 + 一个标签，隔离"框架开销"与"界面内容开销" ---- */
    lv_obj_t *keep = lv_scr_act();
    lv_obj_t *tmp = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(tmp, lv_color_hex(0x101010), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tmp, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tmp, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tmp, 0, LV_PART_MAIN);
    lv_obj_t *lb = lv_label_create(tmp);
    lv_label_set_text(lb, "PERF");
    lv_obj_center(lb);
    lv_scr_load(tmp);

    uint32_t t1 = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < n; i++) {
        lv_obj_invalidate(tmp);
        lv_refr_now(NULL);
    }
    uint32_t plain_ms = ((uint32_t)(esp_timer_get_time() / 1000) - t1) / (uint32_t)n;

    lv_scr_load(keep);
    lv_obj_del(tmp);

    ESP_LOGI(TAG, "PERF plain screen        : %lu ms -> %lu fps (framework floor)",
             (unsigned long)plain_ms, (unsigned long)((plain_ms > 0U) ? (1000U / plain_ms) : 0UL));

    /* ---- ④ 单项控件开销：大字标签 / 圆弧（找出内容里的"耗电大户"）---- */
    /* 4a：40px 大字（5 个字形，抗锯齿混合面积大） */
    lv_obj_t *tmp2 = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(tmp2, lv_color_hex(0x101010), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tmp2, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tmp2, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tmp2, 0, LV_PART_MAIN);
    lv_obj_t *big = lv_label_create(tmp2);
    lv_obj_set_style_text_font(big, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_label_set_text(big, "12:34");
    lv_obj_center(big);
    lv_scr_load(tmp2);

    uint32_t t2 = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < n; i++) {
        lv_obj_invalidate(tmp2);
        lv_refr_now(NULL);
    }
    uint32_t big_ms = ((uint32_t)(esp_timer_get_time() / 1000) - t2) / (uint32_t)n;
    lv_scr_load(keep);            /* 先切回原屏，再删临时屏（删活动屏会崩） */
    lv_obj_del(tmp2);

    /* 4b：圆弧（抗锯齿 + 三角函数逐像素） */
    lv_obj_t *tmp3 = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(tmp3, lv_color_hex(0x101010), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tmp3, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tmp3, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tmp3, 0, LV_PART_MAIN);
    lv_obj_t *arc = lv_arc_create(tmp3);
    lv_obj_set_size(arc, 74, 74);
    lv_obj_center(arc);
    lv_arc_set_range(arc, 0, 600);
    lv_arc_set_value(arc, 250);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0xffb454), LV_PART_INDICATOR);
    lv_scr_load(tmp3);

    uint32_t t3 = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < n; i++) {
        lv_obj_invalidate(tmp3);
        lv_refr_now(NULL);
    }
    uint32_t arc_ms = ((uint32_t)(esp_timer_get_time() / 1000) - t3) / (uint32_t)n;
    lv_scr_load(keep);            /* 同上：先切回再删 */
    lv_obj_del(tmp3);

    ESP_LOGI(TAG, "PERF widget cost         : big-label(+%ld ms) arc(+%ld ms) over plain %lu ms",
             (long)big_ms - (long)plain_ms, (long)arc_ms - (long)plain_ms,
             (unsigned long)plain_ms);
#endif
}

/* ---------------- LVGL 刷新任务 ---------------- */
static void ui_task(void *arg)
{
    BSP_UNUSED(arg);

    ESP_LOGI(TAG, "LVGL task: core=%d", (int)xPortGetCoreID());

    while (s_running) {
        /* lv_timer_handler 返回"距离下一个定时器的毫秒数"，用它做自适应休眠：
         * 有活就连续跑，没活就睡到下一个定时器，CPU 占用接近 0 */
        uint32_t idle = lv_timer_handler();
        if (idle == LV_NO_TIMER_READY || idle > 10U) {
            idle = 10U;
        }
        if (idle == 0U) {
            idle = 1U;
        }
        vTaskDelay(pdMS_TO_TICKS(idle));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---------------- 对外接口 ---------------- */

esp_err_t ui_start(void)
{
    if (s_running) {
        return ESP_OK;                              /* 幂等 */
    }
    if (bsp_lcd_panel_io() == NULL) {
        ESP_LOGE(TAG, "LCD 未初始化（先 bsp_init(BSP_MOD_LCD)）");
        return ESP_ERR_INVALID_STATE;
    }

    lv_init();
    lv_log_register_print_cb(ui_log_cb);

    /* 双缓冲：LVGL 交替渲染两块，flush 异步提交 */
    lv_disp_draw_buf_init(&s_draw_buf, s_buf0, s_buf1, UI_BUF_PIXELS);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res     = BSP_LCD_WIDTH;
    s_disp_drv.ver_res     = BSP_LCD_HEIGHT;
    s_disp_drv.flush_cb    = ui_flush_cb;
    s_disp_drv.draw_buf    = &s_draw_buf;
    s_disp_drv.direct_mode = 0;                     /* 局部刷新 + 双缓冲 */
    s_disp_drv.full_refresh = 0;
    if (lv_disp_drv_register(&s_disp_drv) == NULL) {
        ESP_LOGE(TAG, "lv_disp_drv_register failed");
        return ESP_FAIL;
    }

    /* 完成回调要在第一帧之前挂上 */
    bsp_lcd_set_flush_done_callback(ui_flush_done_isr);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type    = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = ui_input_cb;
    if (lv_indev_drv_register(&s_indev_drv) == NULL) {
        ESP_LOGE(TAG, "lv_indev_drv_register failed");
        return ESP_FAIL;
    }

    lv_timer_create(ui_fps_timer_cb, 1000, NULL);
#if UI_DEBUG_SHOT
    lv_timer_create(ui_debug_shot_cb, 6000, NULL);
#endif
    lv_timer_create(ui_perf_selftest_cb, 3000, NULL);   /* 上电自测一次 */

    /* 界面在创建刷新任务之前构建（此刻 LVGL 单线程，天然安全） */
    ui_dashboard_build();

    /* 首帧：整屏刷一遍，避免上电残留 */
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);

    s_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "lvgl", UI_TASK_STACK, NULL,
                                            UI_TASK_PRIO, &s_task, UI_TASK_CORE);
    if (ok != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "LVGL task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LVGL %d.%d.%d ready: %dx%d, buf=%dx%d x2 (internal RAM), core1",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
             BSP_LCD_WIDTH, BSP_LCD_HEIGHT, BSP_LCD_WIDTH, UI_BUF_LINES);
    return ESP_OK;
}

esp_err_t ui_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }
    s_running = false;
    for (int i = 0; i < 200 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));              /* 等任务自行退出 */
    }
    return (s_task == NULL) ? ESP_OK : ESP_ERR_TIMEOUT;
}

uint32_t ui_get_fps(void)
{
    return s_fps;
}

uint8_t ui_get_page_index(void)
{
    return ui_dashboard_page();
}

/* ================================================================
 *  调试：整屏截图经串口回传（视觉回归用）
 * ================================================================ */
void ui_debug_dump_screen(void)
{
    const size_t bytes = (size_t)BSP_LCD_WIDTH * BSP_LCD_HEIGHT * sizeof(uint16_t);

    if (s_shot == NULL) {
        s_shot = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_shot == NULL) {
            ESP_LOGE(TAG, "shot buffer alloc failed");
            return;
        }
    }

    s_shot_active = true;
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);                     /* 整屏重绘一次，flush_cb 里合成整帧 */
    for (int i = 0; i < 50 && s_buf_busy[0]; i++) {   /* 等最后一块 DMA 落地 */
        (void)bsp_lcd_wait_flush_done(100);
    }
    s_shot_active = false;

    esp_log_level_set("*", ESP_LOG_NONE);   /* 二进制流期间静音所有日志，避免插入破坏 */
    printf("\nDUMP %u %u\n", (unsigned)BSP_LCD_WIDTH, (unsigned)BSP_LCD_HEIGHT);
    fflush(stdout);

    const uint8_t *src = (const uint8_t *)s_shot;
    for (size_t off = 0; off < bytes; off += 512U) {
        size_t n = bytes - off;
        if (n > 512U) {
            n = 512U;
        }
        fwrite(src + off, 1, n, stdout);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1));      /* 让出 CPU，别饿死其它任务 */
    }
    printf("\nDUMP_END\n");
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_INFO);  /* 恢复日志 */
    ESP_LOGI(TAG, "screen dumped (%ux%u, %u B)",
             (unsigned)BSP_LCD_WIDTH, (unsigned)BSP_LCD_HEIGHT, (unsigned)bytes);
}

/* 上电后自动切到 FACE 页并抓一张图（仅调试构建；UI_DEBUG_SHOT=0 即关闭） */

static void ui_debug_shot_cb(lv_timer_t *t)
{
    static int stage;
    lv_timer_t *self = t;
    if (stage == 0) {
        if (ui_dashboard_page() == 0U) {
            ui_dashboard_request(1);       /* HOME -> FACE */
        }
        stage = 1;
        lv_timer_set_period(self, 1500);
        return;
    }
    if (stage == 1) {
        ui_debug_dump_screen();
        stage = 2;
    }
    lv_timer_del(self);
}

void ui_next_page(void)
{
    ui_dashboard_request(1);
}

void ui_prev_page(void)
{
    ui_dashboard_request(-1);
}
