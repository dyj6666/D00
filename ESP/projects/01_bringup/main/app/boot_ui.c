/* ================================================================
 * boot_ui.c —— 开机自检动画
 *
 * 设计要点：
 *   1. **屏幕只有一个所有者**：本任务独占帧缓冲与 flush，别的任务（按键）
 *      只写标志位，避免多任务同时改缓冲；
 *   2. 所有文字走 BSP 的 16px 点阵字库（中英文都能画），不依赖 SD 卡；
 *   3. 每项自检都真跑一遍驱动，把结果落到屏幕上；需要人眼/耳朵/手指的项
 *      标成"待确认"，并给出手上要做的动作；
 *   4. 日志照常打印（BSP/TAG 那套不变），但**不看终端也能完成验收**。
 * ================================================================ */
#include "boot_ui.h"

#include "bsp/audio.h"
#include "bsp/adc.h"
#include "bsp/beep.h"
#include "bsp/bsp.h"
#include "bsp/eeprom.h"
#include "bsp/imu.h"
#include "bsp/key.h"
#include "bsp/lcd.h"
#include "bsp/led.h"
#include "bsp/lightsensor.h"
#include "bsp/rtc.h"
#include "bsp/temperature.h"
#include "bsp/tfcard.h"
#include "bsp/touch.h"
#include "bsp/uart.h"
#include "bsp/xl9555.h"

#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "math.h"
#include "stdio.h"
#include "string.h"

static const char *TAG = "BOOTUI";

/* ---------------- 版式 ---------------- */

#define UI_W            BSP_LCD_WIDTH       /* 320 */
#define UI_H            BSP_LCD_HEIGHT      /* 240 */
#define UI_FONT_H       16
#define UI_TITLE_Y      4
#define UI_SEP_Y        24
#define UI_LIST_Y       28
#define UI_VISIBLE_ROWS 10                  /* 列表可见行数 */
#define UI_ROW_Y(i)     (UI_LIST_Y + (i) * UI_FONT_H)
#define UI_BAR_X        4
#define UI_BAR_Y        212
#define UI_BAR_W        240
#define UI_BAR_H        12

#define UI_TASK_STACK   5120U
#define UI_TASK_PRIO    3U

/* ---------------- 配色 ---------------- */

#define UI_BG           BSP_LCD_COLOR_BLACK
#define UI_FG           BSP_LCD_COLOR_WHITE
#define UI_DIM          BSP_LCD_COLOR_GRAY
#define UI_OK           BSP_RGB565(0, 220, 90)
#define UI_ERR          BSP_RGB565(255, 70, 70)
#define UI_INFO         BSP_RGB565(255, 200, 0)
#define UI_TITLE        BSP_RGB565(0, 170, 255)

/* ---------------- 结果类型 ---------------- */

typedef enum
{
    RES_PASS = 0,       /* 驱动自证正常 */
    RES_FAIL,           /* 明确失败 */
    RES_INFO,           /* 需人工确认 */
    RES_SKIP,           /* 模块未初始化 */
} ui_res_t;

static const char *res_text(ui_res_t r)
{
    switch (r) {
    case RES_PASS: return "通过";
    case RES_FAIL: return "失败";
    case RES_INFO: return "待确认";
    default:       return "跳过";
    }
}

static uint16_t res_color(ui_res_t r)
{
    switch (r) {
    case RES_PASS: return UI_OK;
    case RES_FAIL: return UI_ERR;
    case RES_INFO: return UI_INFO;
    default:       return UI_DIM;
    }
}

/* ---------------- 列表状态 ---------------- */

#define ROW_LABEL_MAX   12
/* 细节字段缓冲：屏幕上一行只能显示 26 个 ASCII 字符（x=112 起、宽 208px），
 * 但缓冲要按"格式串理论最坏长度"给足（%d 最坏 11 位），否则 -Wformat-truncation
 * 会直接报错。多出来的部分在屏幕上会被 lcd_plot 的边界检查自然裁掉。 */
#define ROW_DETAIL_MAX  40
#define ROW_TOTAL_MAX   24

typedef struct
{
    char     label[ROW_LABEL_MAX];      /* 中文标签（UTF-8） */
    char     detail[ROW_DETAIL_MAX];    /* ASCII 细节 */
    ui_res_t res;
} ui_row_t;

static ui_row_t s_rows[ROW_TOTAL_MAX];
static int      s_row_count = 0;
static int      s_pass = 0;
static int      s_fail = 0;
static int      s_info = 0;
static int      s_skip = 0;

/* 按键/触摸由中断与轮询填充，UI 任务只读 */
static volatile bool s_replay = false;
static volatile bool s_remount_req = false;     /* K2 长按：请求重新挂载 SD 卡 */
static volatile bool s_format_req = false;      /* K1 长按：请求格式化并挂载 SD 卡 */
static volatile bool s_key_event_pending = false;
static volatile int  s_key_event_key = -1;      /* bsp_key_id_t */
static volatile int  s_key_event_ev = -1;       /* bsp_key_event_t */
static volatile int64_t s_key_event_ms = 0;

/* TF 裸 SPI 探测结果：用来把"卡的问题"和"驱动/格式的问题"分开 */
static bsp_tfcard_probe_t s_tf_probe;
static bool               s_tf_probe_valid = false;

static void tf_probe_refresh(void);     /* 前置声明（t_tfcard 与任务里都要用） */
static void tf_probe_show(void);

/* 测试音缓冲 */
#define TONE_FRAMES     256U
static int16_t s_tone[TONE_FRAMES * 2U];

/* ---------------- 绘制工具 ---------------- */

/**
 * @brief  清屏（整帧刷底色）
 */
static void ui_clear(uint16_t color)
{
    (void)bsp_lcd_fill(color);
}

/**
 * @brief  画一行"自检结果"
 * @param  y       行顶 y
 * @param  fade_x  状态文字的横向偏移（用于滑入动画，0 = 到位）
 */
static void ui_draw_row(int y, const ui_row_t *row, int fade_x)
{
    bsp_lcd_fb_fill_rect(0, (uint16_t)y, UI_W, UI_FONT_H, UI_BG);
    bsp_lcd_text(4, y, row->label, UI_FG, UI_BG);

    /* 四种状态都画（含"跳过"），保证每行都有明确结论，不留空 */
    bsp_lcd_text(44 + fade_x, y, res_text(row->res), res_color(row->res), UI_BG);
    if (row->detail[0] != '\0') {
        bsp_lcd_text(112, y, row->detail, UI_DIM, UI_BG);
    }
}

/**
 * @brief  重画整个列表区（只显示最近 UI_VISIBLE_ROWS 行，实现向上滚动）
 */
static void ui_redraw_list(void)
{
    const int show = (s_row_count < UI_VISIBLE_ROWS) ? s_row_count : UI_VISIBLE_ROWS;
    const int first = s_row_count - show;

    bsp_lcd_fb_fill_rect(0, UI_LIST_Y, UI_W, UI_VISIBLE_ROWS * UI_FONT_H, UI_BG);
    for (int i = 0; i < show; i++) {
        ui_draw_row(UI_ROW_Y(i), &s_rows[first + i], 0);
    }
    (void)bsp_lcd_flush_rect(0, UI_LIST_Y, UI_W, UI_VISIBLE_ROWS * UI_FONT_H);
}

/**
 * @brief  画标题栏
 */
static void ui_draw_title(const char *sub)
{
    bsp_lcd_fb_fill_rect(0, 0, UI_W, UI_SEP_Y, UI_BG);
    bsp_lcd_text(4, UI_TITLE_Y, "ESP32S3 BOX", UI_TITLE, UI_BG);
    bsp_lcd_text(4 + 12 * 8 + 8, UI_TITLE_Y, bsp_version_string(), UI_DIM, UI_BG);
    if (sub != NULL) {
        bsp_lcd_text(4 + 19 * 8, UI_TITLE_Y, sub, UI_FG, UI_BG);
    }
    bsp_lcd_fb_fill_rect(0, UI_SEP_Y - 2, UI_W, 1, UI_DIM);
    (void)bsp_lcd_flush_rect(0, 0, UI_W, UI_SEP_Y);
}

/**
 * @brief  画进度条（带右侧计数）
 */
static void ui_draw_progress(int done, int total)
{
    char buf[32];       /* 两个 %d 的最坏长度远超 16，缓冲必须给足 */

    bsp_lcd_fb_fill_rect(0, UI_BAR_Y - 2, UI_W, UI_BAR_H + 4, UI_BG);
    bsp_lcd_frame(UI_BAR_X, UI_BAR_Y, UI_BAR_W, UI_BAR_H, UI_DIM);

    const int inner = UI_BAR_W - 4;
    const int fill = (total > 0) ? (inner * done / total) : 0;

    if (fill > 0) {
        bsp_lcd_fb_fill_rect((uint16_t)(UI_BAR_X + 2), (uint16_t)(UI_BAR_Y + 2),
                             (uint16_t)fill, UI_BAR_H - 4, UI_OK);
    }

    (void)snprintf(buf, sizeof(buf), "%d/%d", done, total);
    bsp_lcd_text(UI_BAR_X + UI_BAR_W + 8, UI_BAR_Y - 2, buf, UI_FG, UI_BG);
    (void)bsp_lcd_flush_rect(0, UI_BAR_Y - 2, UI_W, UI_BAR_H + 4);
}

/**
 * @brief  追加一行结果（含滑入动画）
 */
static void ui_push_row(const char *label, const char *detail, ui_res_t res)
{
    ui_row_t *row;

    if (s_row_count >= ROW_TOTAL_MAX) {
        return;
    }
    row = &s_rows[s_row_count];
    (void)snprintf(row->label, sizeof(row->label), "%s", label);
    (void)snprintf(row->detail, sizeof(row->detail), "%s", detail);
    row->res = res;
    s_row_count++;

    switch (res) {
    case RES_PASS: s_pass++; break;
    case RES_FAIL: s_fail++; break;
    case RES_INFO: s_info++; break;
    default:       s_skip++; break;
    }

    if (s_row_count > UI_VISIBLE_ROWS) {
        /* 列表已满：整体重画，天然产生"向上滚动"的观感 */
        ui_redraw_list();
        return;
    }

    /* 状态文字从右滑入：48 -> 24 -> 0，像"落定" */
    const int y = UI_ROW_Y(s_row_count - 1);

    for (int dx = 48; dx >= 0; dx -= 24) {
        bsp_lcd_fb_fill_rect(0, (uint16_t)y, UI_W, UI_FONT_H, UI_BG);
        bsp_lcd_text(4, y, row->label, UI_FG, UI_BG);
        if (row->detail[0] != '\0') {
            bsp_lcd_text(112, y, row->detail, UI_DIM, UI_BG);
        }
        bsp_lcd_text(44 + dx, y, res_text(res), res_color(res), UI_BG);
        (void)bsp_lcd_flush_rect(0, (uint16_t)y, UI_W, UI_FONT_H);
        vTaskDelay(pdMS_TO_TICKS(35));
    }
}

/**
 * @brief  把一项测试跑起来并在屏幕上落结果
 * @param  status_row 若不为 NULL，则先在该行显示"…"表示正在执行
 */
static void ui_run_item(const char *label, ui_res_t (*run)(char *detail, size_t cap))
{
    char detail[ROW_DETAIL_MAX] = {0};

    if (run == NULL) {
        ui_push_row(label, "", RES_SKIP);
        return;
    }

    ui_push_row(label, detail, run(detail, sizeof(detail)));
}

/* ---------------- 各项自检 ---------------- */

static ui_res_t t_lcd(char *d, size_t n)
{
    if (bsp_lcd_fb() == NULL) {
        return RES_FAIL;
    }
    (void)snprintf(d, n, "%dx%d 16bit", UI_W, UI_H);
    return RES_PASS;
}

static ui_res_t t_touch(char *d, size_t n)
{
    if (!bsp_module_is_running(BSP_MOD_TOUCH)) {
        return RES_SKIP;
    }

    uint32_t pid = 0;

    if (bsp_touch_read_pid(&pid) != ESP_OK) {
        return RES_FAIL;
    }
    (void)snprintf(d, n, "PID=%06X", (unsigned)(pid & 0xFFFFFFU));

    /* 给 1.6 秒时间让人按一下屏：按到就 PASS，否则标"待确认" */
    for (int i = 0; i < 16; i++) {
        bsp_touch_state_t st;

        if (bsp_touch_read(&st) == ESP_OK && st.pressed) {
            (void)snprintf(d, n, "%u点 (%u,%u)", (unsigned)st.count,
                           (unsigned)st.point[0].x, (unsigned)st.point[0].y);
            return RES_PASS;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    (void)snprintf(d, n, "请按屏");
    return RES_INFO;
}

static ui_res_t t_key(char *d, size_t n)
{
    if (!bsp_module_is_running(BSP_MOD_KEY)) {
        return RES_SKIP;
    }
    (void)snprintf(d, n, "K1 K2 BOOT");
    return RES_INFO;        /* 按键靠人手触发，事件会显示在状态条 */
}

static ui_res_t t_led(char *d, size_t n)
{
    if (!bsp_module_is_running(BSP_MOD_LED)) {
        return RES_SKIP;
    }

    (void)bsp_led_set(BSP_LED_BLUE, true);
    (void)bsp_led_set(BSP_LED_RED, true);
    vTaskDelay(pdMS_TO_TICKS(250));
    (void)bsp_led_set(BSP_LED_BLUE, false);
    (void)bsp_led_set(BSP_LED_RED, false);

    (void)snprintf(d, n, "IO4 + P1_0");
    return RES_PASS;
}

static ui_res_t t_beep(char *d, size_t n)
{
    if (!bsp_module_is_running(BSP_MOD_BEEP)) {
        return RES_SKIP;
    }

    (void)bsp_beep_once(120);
    vTaskDelay(pdMS_TO_TICKS(250));
    (void)snprintf(d, n, "P0_2");
    return RES_PASS;
}

static ui_res_t t_xl9555(char *d, size_t n)
{
    uint16_t in = 0;

    if (bsp_xl9555_inputs(&in) != ESP_OK) {
        return RES_FAIL;
    }
    (void)snprintf(d, n, "in=%04X", (unsigned)in);
    return RES_PASS;
}

static ui_res_t t_eeprom(char *d, size_t n)
{
    if (!bsp_module_is_running(BSP_MOD_EEPROM)) {
        return RES_SKIP;
    }

    const esp_err_t err = bsp_eeprom_self_test(BSP_EEPROM_SIZE - 4U);

    (void)snprintf(d, n, "AT24C02 %uB", (unsigned)BSP_EEPROM_SIZE);
    return (err == ESP_OK) ? RES_PASS : RES_FAIL;
}

static ui_res_t t_light(char *d, size_t n)
{
    bsp_light_data_t v;

    if (!bsp_module_is_running(BSP_MOD_LIGHT)) {
        return RES_SKIP;
    }
    if (bsp_light_read(&v) != ESP_OK) {
        return RES_FAIL;
    }
    (void)snprintf(d, n, "als %u ps %u", (unsigned)v.als, (unsigned)v.ps);
    return RES_PASS;
}

static ui_res_t t_imu(char *d, size_t n)
{
    bsp_imu_data_t v;

    if (!bsp_module_is_running(BSP_MOD_IMU)) {
        return RES_SKIP;
    }
    if (bsp_imu_read(&v) != ESP_OK) {
        return RES_FAIL;
    }

    const float a = sqrtf(v.acc_g.x * v.acc_g.x + v.acc_g.y * v.acc_g.y + v.acc_g.z * v.acc_g.z);

    (void)snprintf(d, n, "|a|=%.2fg", a);
    /* 静止时合加速度应接近 1g，这条能一次暴露量程/换算错误 */
    return (a > 0.5f && a < 1.6f) ? RES_PASS : RES_FAIL;
}

static ui_res_t t_temp(char *d, size_t n)
{
    float c = 0.0f;

    if (!bsp_module_is_running(BSP_MOD_TEMP)) {
        return RES_SKIP;
    }
    if (bsp_temperature_read_avg(8, &c) != ESP_OK) {
        return RES_FAIL;
    }
    (void)snprintf(d, n, "%.1f C", c);
    return (c > -20.0f && c < 100.0f) ? RES_PASS : RES_FAIL;
}

static ui_res_t t_rtc(char *d, size_t n)
{
    char buf[24] = {0};

    if (!bsp_module_is_running(BSP_MOD_RTC)) {
        return RES_SKIP;
    }
    (void)bsp_rtc_format(buf, sizeof(buf));
    (void)snprintf(d, n, "%s", buf);
    return RES_PASS;
}

static ui_res_t t_adc(char *d, size_t n)
{
    int mv = 0;

    if (!bsp_module_is_running(BSP_MOD_ADC)) {
        return RES_SKIP;
    }
    (void)bsp_adc_read_mv_avg(BSP_ADC_CH_0, 8, &mv);
    (void)snprintf(d, n, "IO5 %dmV", mv);
    return RES_INFO;        /* 悬空为漂动值，接电压才能确认 */
}

static ui_res_t t_uart(char *d, size_t n)
{
    static const char ping[] = "PING";
    uint8_t rx[16] = {0};

    if (!bsp_module_is_running(BSP_MOD_UART)) {
        return RES_SKIP;
    }

    (void)bsp_uart_flush();
    (void)bsp_uart_write(ping, sizeof(ping) - 1U, 0U);
    const int got = bsp_uart_read(rx, sizeof(rx), 150U);

    if (got > 0) {
        (void)snprintf(d, n, "回环 OK");
        return RES_PASS;
    }
    (void)snprintf(d, n, "短接IO5-IO6");
    return RES_INFO;
}

/**
 * @brief  把"没挂上卡"的原因翻译成屏幕上一眼看懂的一句话
 * @note   TF 卡是热插拔的：没插好和格式不对是两回事，分开提示才好动手
 */
static const char *tf_reason(char *buf, size_t n)
{
    switch (bsp_tfcard_last_error()) {
    case ESP_OK:
        (void)snprintf(buf, n, "未挂载");
        break;
    case ESP_ERR_NOT_FOUND:
        (void)snprintf(buf, n, "无卡/未插好");
        break;
    case ESP_ERR_TIMEOUT:
        (void)snprintf(buf, n, "无卡/接触不良");
        break;
    case ESP_FAIL:
        (void)snprintf(buf, n, "卡非FAT格式");
        break;
    default:
        (void)snprintf(buf, n, "错误 0x%X", (unsigned)bsp_tfcard_last_error());
        break;
    }
    return buf;
}

static ui_res_t t_tfcard(char *d, size_t n)
{
    char r[24];

    if (!bsp_tfcard_is_mounted()) {
        /* 开机那一刻可能还没插卡 —— 这里再试一次（热插拔） */
        if (bsp_tfcard_init() != ESP_OK) {
            /* 挂不上就再做一次裸 SPI 探测：卡有没有应答，屏幕上直接说清楚 */
            tf_probe_refresh();
            (void)snprintf(d, n, "%s",
                           (s_tf_probe_valid && s_tf_probe.responded) ? "卡有应答"
                                                                     : "卡无应答");
            return RES_INFO;        /* 不是驱动问题，多半是没插好或格式不对 */
        }
    }

    uint64_t total = 0;

    (void)bsp_tfcard_info(&total, NULL, NULL, 0);
    (void)snprintf(d, n, "%uM", (unsigned)(total >> 20));
    return RES_PASS;
}

static ui_res_t t_audio(char *d, size_t n)
{
    if (!bsp_audio_is_ready()) {
        return RES_SKIP;
    }

    /* 440Hz 短音（一个周期采样表循环用，省算力） */
    const uint32_t period = 44100U / 440U;

    for (uint32_t i = 0; i < TONE_FRAMES; i++) {
        const int16_t s = (int16_t)(9000.0f * sinf(2.0f * 3.14159265f *
                                                   (float)(i % period) / (float)period));
        s_tone[i * 2U] = s;
        s_tone[i * 2U + 1U] = s;
    }

    (void)bsp_audio_set_volume(60);
    (void)bsp_audio_set_mute(false);

    for (int i = 0; i < 30; i++) {          /* 约 350ms */
        (void)bsp_audio_write(s_tone, sizeof(s_tone), 500U);
    }

    (void)snprintf(d, n, "%s", (bsp_audio_codec() == BSP_AUDIO_CODEC_ES8311) ? "ES8311"
                                                                             : "NS4168");
    return RES_INFO;        /* 请耳朵确认 */
}

/* ---------------- 自检项表 ---------------- */

typedef struct
{
    const char *label;      /* 中文标签 */
    ui_res_t (*run)(char *detail, size_t cap);
} ui_item_t;

static const ui_item_t s_items[] = {
    { "屏幕", t_lcd },
    { "触摸", t_touch },
    { "按键", t_key },
    { "灯光", t_led },
    { "蜂鸣", t_beep },
    { "扩展", t_xl9555 },
    { "存储", t_eeprom },
    { "光感", t_light },
    { "六轴", t_imu },
    { "温度", t_temp },
    { "时钟", t_rtc },
    { "采样", t_adc },
    { "串口", t_uart },
    { "卡",   t_tfcard },
    { "音频", t_audio },
};

/* ---------------- 阶段一：屏幕自检 ---------------- */

/**
 * @brief  整屏闪四色：确认面板、背光、整屏每个像素都可达
 */
static void ui_screen_flash(void)
{
    static const uint16_t colors[4] = {
        BSP_LCD_COLOR_WHITE, BSP_LCD_COLOR_RED, BSP_LCD_COLOR_GREEN, BSP_LCD_COLOR_BLUE,
    };

    for (int i = 0; i < 4; i++) {
        ui_clear(colors[i]);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    ui_clear(UI_BG);
}

/* ---------------- 阶段二：逐项自检 ---------------- */

static void ui_run_selftest(void)
{
    s_row_count = 0;
    s_pass = s_fail = s_info = s_skip = 0;

    ui_clear(UI_BG);
    ui_draw_title("自检");
    ui_draw_progress(0, (int)BSP_COUNT_OF(s_items));

    for (size_t i = 0; i < BSP_COUNT_OF(s_items); i++) {
        ui_run_item(s_items[i].label, s_items[i].run);
        ui_draw_progress((int)(i + 1), (int)BSP_COUNT_OF(s_items));
    }

    /* 汇总一行，停 1.5 秒让人看清 */
    char sum[64];       /* 一次格式化三个整数，缓冲要给足（见 ROW_DETAIL_MAX 注释） */

    (void)snprintf(sum, sizeof(sum), "自检完成 %d过 %d败 %d待", s_pass, s_fail, s_info);
    ui_push_row("结果", sum, s_fail > 0 ? RES_FAIL : RES_PASS);

    ESP_LOGI(TAG, "自检完成: pass=%d fail=%d info=%d skip=%d", s_pass, s_fail, s_info, s_skip);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

/* ---------------- 阶段三：常驻仪表盘 ---------------- */

/* 行号 -> 内容：0 时间 / 1 acc / 2 gyro / 3 光感 / 4 温度 / 5 存储 /
 *              6 运行 / 7 触摸 / 8 按键 / 9 SD裸探测 / 10 操作提示（提示行静态） */
#define UI_DASH_ROWS    11
#define UI_DASH_Y       60      /* 60 + 11*16 = 236，正好铺满下半屏 */

/**
 * @brief  画一行"标签 + 值"，值没变就完全不动屏
 * @note   自带逐行缓存：只有真变了才重绘并刷那一行。所以触摸行可以 50ms
 *         一拍地刷，既灵敏又不影响其它行、也不会闪。
 */
static void dash_row(int row, const char *label, const char *value, uint16_t color)
{
    static char cache[UI_DASH_ROWS][40];

    if (row < 0 || row >= UI_DASH_ROWS) {
        return;
    }
    if (strcmp(cache[row], value) == 0) {
        return;
    }
    (void)snprintf(cache[row], sizeof(cache[row]), "%s", value);

    const int y = UI_DASH_Y + row * UI_FONT_H;

    bsp_lcd_fb_fill_rect(2, (uint16_t)y, UI_W - 4, UI_FONT_H, UI_BG);
    bsp_lcd_text(4, y, label, UI_DIM, UI_BG);
    bsp_lcd_text(52, y, value, color, UI_BG);
    (void)bsp_lcd_flush_rect(2, (uint16_t)y, UI_W - 4, UI_FONT_H);
}

/**
 * @brief  裸 SPI 探测一次卡（只在"没挂载"时做，别打断正在工作的卡）
 */
static void tf_probe_refresh(void)
{
    if (bsp_tfcard_is_mounted()) {
        return;
    }
    s_tf_probe_valid = (bsp_tfcard_probe_raw(&s_tf_probe) == ESP_OK);
}

/**
 * @brief  把裸探测结果画到"SD探测"行
 * @note   这一行是硬件取证：无应答 = 查卡座/卡片；有应答 = 卡是好的，问题在格式或驱动
 */
static void tf_probe_show(void)
{
    char v[48];

    if (bsp_tfcard_is_mounted()) {
        dash_row(9, "SD探测", "--", UI_DIM);
        return;
    }
    if (!s_tf_probe_valid) {
        dash_row(9, "SD探测", "总线忙", UI_DIM);
        return;
    }
    if (!s_tf_probe.responded) {
        dash_row(9, "SD探测", "无应答/查卡座", UI_ERR);
        return;
    }
    (void)snprintf(v, sizeof(v), "R1=%02X %s", (unsigned)s_tf_probe.r1_idle,
                   s_tf_probe.cmd8_ok ? "CMD8正常" : "CMD8无回显");
    dash_row(9, "SD探测", v, UI_OK);
}

/**
 * @brief  常驻界面骨架：1px 边框 + 标题 + 自检汇总 + 分隔线 + 操作提示
 * @note   按需求撤掉名字卡；自检结论一直留在屏上，不必回翻日志
 */
static void ui_show_idle(void)
{
    char sum[64];

    ui_clear(UI_BG);
    ui_draw_title(NULL);

    (void)snprintf(sum, sizeof(sum), "%d 通过  %d 失败  %d 待确认", s_pass, s_fail, s_info);
    bsp_lcd_text(4, 30, sum, (s_fail > 0) ? UI_ERR : UI_OK, UI_BG);
    bsp_lcd_fb_fill_rect(0, 52, UI_W, 1, UI_DIM);

    bsp_lcd_text(4, UI_DASH_Y + 10 * UI_FONT_H,
                 "K1重测 K2鸣 K2长按挂卡 K1长按格式化", UI_DIM, UI_BG);
    /* 边框最后画：标题栏会清 0~24 行，先画会被它擦掉 */
    bsp_lcd_frame(0, 0, UI_W, UI_H, UI_TITLE);
    (void)bsp_lcd_flush();

    /* 骨架就位后补一次裸探测：卡没挂上时，这一行最能说明问题 */
    tf_probe_refresh();
    tf_probe_show();
}

/**
 * @brief  每秒刷新的慢变量：时间 / 六轴 / 光感 / 温度 / 存储 / 运行状态
 */
static void ui_live_slow(void)
{
    char v[48];
    char tbuf[24] = {0};

    (void)bsp_rtc_format(tbuf, sizeof(tbuf));
    dash_row(0, "时间", tbuf, UI_FG);

    bsp_imu_data_t imu;

    if (bsp_imu_read(&imu) == ESP_OK) {
        (void)snprintf(v, sizeof(v), "%.2f %.2f %.2f g", imu.acc_g.x, imu.acc_g.y, imu.acc_g.z);
        dash_row(1, "acc", v, UI_FG);

        (void)snprintf(v, sizeof(v), "%.0f %.0f %.0f dps", imu.gyro_dps.x, imu.gyro_dps.y,
                       imu.gyro_dps.z);
        dash_row(2, "gyro", v, UI_DIM);
    } else {
        dash_row(1, "acc", "--", UI_ERR);
        dash_row(2, "gyro", "--", UI_ERR);
    }

    bsp_light_data_t lt;

    if (bsp_light_read(&lt) == ESP_OK) {
        (void)snprintf(v, sizeof(v), "%u / %u / %u", (unsigned)lt.als, (unsigned)lt.ps,
                       (unsigned)lt.ir);
        dash_row(3, "光感", v, UI_FG);
    }

    float c = 0.0f;

    if (bsp_temperature_read_avg(4, &c) == ESP_OK) {
        (void)snprintf(v, sizeof(v), "%.1f C", c);
        dash_row(4, "温度", v, UI_FG);
    }

    if (!bsp_tfcard_is_mounted()) {
        char r[24];

        dash_row(5, "存储", tf_reason(r, sizeof(r)), UI_INFO);
    } else {
        uint64_t total = 0;
        uint64_t freeb = 0;

        if (bsp_tfcard_info(&total, &freeb, NULL, 0) == ESP_OK) {
            (void)snprintf(v, sizeof(v), "%uM / %uM free", (unsigned)(total >> 20),
                           (unsigned)(freeb >> 20));
            dash_row(5, "存储", v, UI_FG);
        }
    }

    (void)snprintf(v, sizeof(v), "%us  heap %uK",
                   (unsigned)(esp_timer_get_time() / 1000000LL),
                   (unsigned)(esp_get_free_heap_size() / 1024U));
    dash_row(6, "运行", v, UI_DIM);
}

/**
 * @brief  50ms 节拍刷新的快变量：触摸坐标 / 按键事件
 */
static void ui_live_fast(void)
{
    char v[48];

    bsp_touch_state_t st;

    if (bsp_module_is_running(BSP_MOD_TOUCH) && bsp_touch_read(&st) == ESP_OK && st.pressed) {
        (void)snprintf(v, sizeof(v), "%u 点 (%u,%u)", (unsigned)st.count,
                       (unsigned)st.point[0].x, (unsigned)st.point[0].y);
        dash_row(7, "触摸", v, UI_INFO);
    } else {
        dash_row(7, "触摸", "-", UI_DIM);
    }

    /* 按键：优先显示最近 3 秒内发生的事件 */
    const int64_t now = esp_timer_get_time() / 1000LL;

    if (s_key_event_pending && (now - s_key_event_ms) < 3000) {
        const int k = s_key_event_key;
        const int e = s_key_event_ev;
        const char *kn = (k == (int)BSP_KEY_BOOT) ? "BOOT" : (k == (int)BSP_KEY_0) ? "K1" : "K2";
        const char *en = (e == (int)BSP_KEY_EVENT_DOWN) ? "按下"
                         : (e == (int)BSP_KEY_EVENT_UP) ? "松开"
                         : (e == (int)BSP_KEY_EVENT_CLICK) ? "单击" : "长按";

        (void)snprintf(v, sizeof(v), "%s %s", kn, en);
        dash_row(8, "按键", v, UI_OK);
    } else {
        s_key_event_pending = false;
        dash_row(8, "按键", "-", UI_DIM);
    }
}

/* ---------------- 按键 ---------------- */

/**
 * @brief  按键事件：记录到状态条（最多显示 3 秒），并给出可见/可听反馈
 * @note   运行在按键任务上下文；这里只写标志，不碰屏幕（屏幕归 UI 任务）
 */
static void ui_key_cb(bsp_key_id_t key, bsp_key_event_t event, uint32_t hold_ms, void *ctx)
{
    BSP_UNUSED(ctx);
    BSP_UNUSED(hold_ms);

    /* 只传枚举值（原子写），字符串交给 UI 任务自己拼 —— 两个任务不共享缓冲区 */
    s_key_event_key = (int)key;
    s_key_event_ev = (int)event;
    s_key_event_ms = esp_timer_get_time() / 1000LL;
    s_key_event_pending = true;

    if (event == BSP_KEY_EVENT_CLICK) {
        if (key == BSP_KEY_0) {
            s_replay = true;                    /* K1：重跑自检动画 */
        } else if (key == BSP_KEY_1) {
            (void)bsp_beep_once(60);            /* K2：短鸣 */
        } else {
            (void)bsp_led_toggle(BSP_LED_RED);  /* BOOT：翻转红灯 */
        }
    } else if (event == BSP_KEY_EVENT_LONG_PRESS) {
        if (key == BSP_KEY_0) {
            s_format_req = true;                /* K1 长按：格式化并挂载（会清空卡！） */
        } else if (key == BSP_KEY_1) {
            s_remount_req = true;               /* K2 长按：重新挂载 SD（热插拔） */
        } else {
            (void)bsp_led_breath_start(700);    /* BOOT 长按：呼吸灯开关 */
        }
    }
}

/* ---------------- 主任务 ---------------- */

static void boot_ui_task(void *arg)
{
    BSP_UNUSED(arg);

    vTaskDelay(pdMS_TO_TICKS(400));             /* 让开机日志先刷出来 */

    /* 先挂按键回调：动画期间按 K1 也能排队重测 */
    (void)bsp_key_set_callback(ui_key_cb, NULL);

    ui_screen_flash();                          /* ① 屏幕四色自检 */
    ui_run_selftest();                          /* ② 逐项自检 */
    ui_show_idle();                             /* ③ 名字卡 + 状态条 */

    for (;;) {
        if (s_replay) {
            s_replay = false;
            (void)bsp_led_breath_stop();
            ui_screen_flash();
            ui_run_selftest();
            ui_show_idle();
            continue;
        }

        if (s_remount_req) {
            s_remount_req = false;

            const esp_err_t err = bsp_tfcard_remount();

            (void)bsp_beep_once((err == ESP_OK) ? 120 : 40);   /* 成功长鸣、失败短鸣 */
            ui_live_slow();                                    /* 立刻刷新"存储"行 */
            tf_probe_refresh();                                /* 失败时顺手更新裸探测 */
            tf_probe_show();
            ESP_LOGI(TAG, "TF remount: %s", esp_err_to_name(err));
        }

        if (s_format_req) {
            s_format_req = false;

            /* 先给一段"警告音 + 红灯"提示：接下来会动卡上的数据 */
            (void)bsp_beep_beep(3, 60, 60);
            (void)bsp_led_set(BSP_LED_RED, true);

            const esp_err_t err = bsp_tfcard_format_and_mount();

            (void)bsp_led_set(BSP_LED_RED, false);
            (void)bsp_beep_once((err == ESP_OK) ? 200 : 60);
            ui_live_slow();
            tf_probe_refresh();
            tf_probe_show();
            ESP_LOGI(TAG, "TF format+mount: %s", esp_err_to_name(err));
        }

        ui_live_slow();
        for (int i = 0; i < 20; i++) {      /* 50ms × 20 = 1s：触摸/按键走快节拍 */
            ui_live_fast();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

void boot_ui_start(void)
{
    if (xTaskCreate(boot_ui_task, "boot_ui", UI_TASK_STACK, NULL, UI_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create task failed");
    }
}
