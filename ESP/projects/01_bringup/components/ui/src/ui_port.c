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

/* ---------------- 显示缓冲（内部 RAM，DMA 可达） ---------------- */
static lv_color_t s_buf0[UI_BUF_PIXELS] __attribute__((aligned(8)));
static lv_color_t s_buf1[UI_BUF_PIXELS] __attribute__((aligned(8)));

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_indev_drv;

static volatile bool     s_flush_busy  = false;  /* 有一笔 DMA 在途 */
static volatile uint32_t s_frame_count = 0;      /* 统计窗口内完成的上屏次数 */
static uint32_t          s_fps         = 0;
static volatile bool     s_running     = false;
static TaskHandle_t      s_task        = NULL;

/* ---------------- 日志转发到 ESP_LOG ---------------- */
static void ui_log_cb(const char *buf)
{
    ESP_LOGW("LVGL", "%s", buf);
}

/* ---------------- 传输完成（ISR 上下文） ---------------- */
static void IRAM_ATTR ui_flush_done_isr(void)
{
    s_flush_busy = false;
    s_frame_count++;
    /* 立刻放行 LVGL：允许它复用刚搬完的缓冲，渲染流水线不中断 */
    lv_disp_flush_ready(&s_disp_drv);
}

/* ---------------- 显示刷新回调 ---------------- */
static void ui_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px_map)
{
    /* 单笔在途约束：上一笔没搬完就等它（用 BSP 的信号量，CPU 让出） */
    while (s_flush_busy) {
        (void)bsp_lcd_wait_flush_done(100);
    }

    s_flush_busy = true;
    esp_err_t err = bsp_lcd_draw_bitmap_async((uint16_t)area->x1, (uint16_t)area->y1,
                                              (uint16_t)(area->x2 + 1), (uint16_t)(area->y2 + 1),
                                              (const uint16_t *)px_map);
    if (err != ESP_OK) {
        s_flush_busy = false;
        lv_disp_flush_ready(drv);          /* 提交失败也必须放行，否则 LVGL 卡死 */
    }
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

    const int n = 20;
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < n; i++) {
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
    }
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000) - t0;
    uint32_t per = ms / (uint32_t)n;

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);

    ESP_LOGI(TAG, "PERF full-screen redraw: %lu ms/frame -> %lu fps (bus limit ~65 fps @10MHz)",
             (unsigned long)per, (unsigned long)((per > 0U) ? (1000U / per) : 0UL));
    ESP_LOGI(TAG, "PERF mem: internal heap free=%u B | LVGL heap %u%% used (%u/%u B)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)mon.used_pct,
             (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.total_size);
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

void ui_next_page(void)
{
    ui_dashboard_request(1);
}

void ui_prev_page(void)
{
    ui_dashboard_request(-1);
}
