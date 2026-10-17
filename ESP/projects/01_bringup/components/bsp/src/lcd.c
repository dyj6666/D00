/* ================================================================
 * lcd.c —— 2.4 寸 320x240 TFT（8080 8bit 并口 + ST7789VW）
 *
 * 关键点（都是踩过坑的）：
 *   1. 背光挂在 XL9555 P0_7，必须先用 I2C 打开，否则屏全黑；
 *   2. 8bit 并口一个像素分两次写，面板 IO 必须开 swap_color_bytes，
 *      缓冲区里放标准 RGB565 即可，不用自己倒字节；
 *   3. 该屏需要 invert_color(true) + MADCTL=0x00 + COLMOD=0x65；
 *   4. 横屏 = swap_xy(true) + mirror(true,false)，与厂商例程一致。
 * ================================================================ */
#include "bsp/lcd.h"

#include "bsp/bsp_pinmap.h"
#include "bsp/ui_font.h"
#include "bsp/xl9555.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdarg.h>
#include <stdio.h>

static const char *TAG = "BSP/LCD";

/* ---- ST7789 私有寄存器 ---- */
#define LCD_CMD_MADCTL          0x36U       /* 内存访问方向 */
#define LCD_CMD_COLMOD          0x3AU       /* 像素格式 */

#define LCD_MADCTL_VALUE        0x00U
#define LCD_COLMOD_VALUE        0x65U       /* 16bit/pixel RGB565 */

#define LCD_TRANS_QUEUE_DEPTH   10U
#define LCD_FB_ALIGN            64U         /* 与总线 psram_trans_align 对应 */

static esp_lcd_panel_handle_t    s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_i80_bus_handle_t  s_i80_bus = NULL;
static SemaphoreHandle_t         s_flush_sem = NULL;
static uint16_t                 *s_fb = NULL;
static bool                      s_backlight_on = false;

/* 应用层完成回调（ISR 上下文调用；LVGL 用它做渲染/DMA 重叠） */
static bsp_lcd_flush_done_cb_t    s_done_cb = NULL;

/* ---------------- 内部函数声明 ---------------- */

static esp_err_t lcd_new_bus_and_panel(void);
static void      lcd_apply_landscape(void);
static void      lcd_config_rd_pin(void);

/* ---------------- 刷新完成回调 ---------------- */

/**
 * @brief  颜色传输完成回调（中断上下文）
 */
static bool IRAM_ATTR lcd_color_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                              esp_lcd_panel_io_event_data_t *edata,
                                              void *user_ctx)
{
    BSP_UNUSED(panel_io);
    BSP_UNUSED(edata);
    BSP_UNUSED(user_ctx);

    BaseType_t woken = pdFALSE;

    if (s_flush_sem != NULL) {
        xSemaphoreGiveFromISR(s_flush_sem, &woken);
    }

    /* 应用层（LVGL）的完成通知：必须在 ISR 里立刻回调，
     * 这样"渲染下一块"可以与"本块 DMA 搬运"完整重叠。 */
    if (s_done_cb != NULL) {
        s_done_cb();
    }

    return (woken == pdTRUE);
}

/* ---------------- 生命周期 ---------------- */

esp_err_t bsp_lcd_init(void)
{
    if (s_panel != NULL) {
        return ESP_OK;                          /* 幂等 */
    }

    s_flush_sem = xSemaphoreCreateBinary();
    if (s_flush_sem == NULL) {
        ESP_LOGE(TAG, "create flush semaphore failed");
        return ESP_ERR_NO_MEM;
    }

    /* ① RD 引脚：只写不读，但硬件要求保持高电平 */
    lcd_config_rd_pin();

    /* ② 8080 总线 + 面板 IO + ST7789 */
    esp_err_t err = lcd_new_bus_and_panel();
    if (err != ESP_OK) {
        return err;
    }

    /* ③ 面板基础初始化（顺序与厂商例程一致） */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 0));

    const uint8_t madctl = LCD_MADCTL_VALUE;
    const uint8_t colmod = LCD_COLMOD_VALUE;
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_MADCTL, &madctl, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_COLMOD, &colmod, 1));

    /* ④ 横屏 */
    lcd_apply_landscape();
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* ⑤ 申请整帧帧缓冲（PSRAM，64 字节对齐） */
    const size_t fb_bytes = (size_t)BSP_LCD_WIDTH * BSP_LCD_HEIGHT * sizeof(uint16_t);

    s_fb = heap_caps_aligned_alloc(LCD_FB_ALIGN, fb_bytes, MALLOC_CAP_SPIRAM);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "framebuffer alloc failed (%u bytes)", (unsigned)fb_bytes);
        return ESP_ERR_NO_MEM;
    }
    bsp_lcd_fb_fill_rect(0, 0, BSP_LCD_WIDTH, BSP_LCD_HEIGHT, 0x0000);

    /* ⑥ 开背光：此刻屏幕才开始可见 */
    err = bsp_lcd_set_backlight(true);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "ready %dx%d (ST7789 8bit-I80 %uMHz, fb=%uKB PSRAM)",
             BSP_LCD_WIDTH, BSP_LCD_HEIGHT,
             (unsigned)(BSP_LCD_PCLK_HZ / 1000000U), (unsigned)(fb_bytes / 1024U));
    return ESP_OK;
}

esp_err_t bsp_lcd_deinit(void)
{
    if (s_panel == NULL) {
        return ESP_OK;
    }

    (void)bsp_lcd_set_backlight(false);

    if (s_fb != NULL) {
        heap_caps_free(s_fb);
        s_fb = NULL;
    }

    /* 释放顺序（与创建顺序相反）：面板 -> 面板 IO -> i80 总线 */
    esp_err_t err = esp_lcd_panel_del(s_panel);
    s_panel = NULL;

    if (s_io != NULL) {
        esp_err_t io_err = esp_lcd_panel_io_del(s_io);

        if (err == ESP_OK) {
            err = io_err;
        }
        s_io = NULL;
    }

    if (s_i80_bus != NULL) {
        esp_err_t bus_err = esp_lcd_del_i80_bus(s_i80_bus);

        if (err == ESP_OK) {
            err = bus_err;
        }
        s_i80_bus = NULL;
    }
    return err;
}

/* ---------------- 背光 ---------------- */

esp_err_t bsp_lcd_set_backlight(bool on)
{
    esp_err_t err = bsp_xl9555_pin_write(BSP_XL9555_PIN_LCD_BL, on);
    if (err == ESP_OK) {
        s_backlight_on = on;
    } else {
        ESP_LOGE(TAG, "backlight %s failed: %s", on ? "on" : "off", esp_err_to_name(err));
    }
    return err;
}

bool bsp_lcd_backlight_is_on(void)
{
    return s_backlight_on;
}

/* ---------------- 帧缓冲 ---------------- */

uint16_t *bsp_lcd_fb(void)
{
    return s_fb;
}

void bsp_lcd_fb_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (s_fb == NULL || w == 0U || h == 0U) {
        return;
    }

    const uint16_t x_end = (uint16_t)BSP_MIN((uint32_t)x + w, (uint32_t)BSP_LCD_WIDTH);
    const uint16_t y_end = (uint16_t)BSP_MIN((uint32_t)y + h, (uint32_t)BSP_LCD_HEIGHT);

    for (uint16_t row = y; row < y_end; row++) {
        uint16_t *dst = s_fb + (size_t)row * BSP_LCD_WIDTH + x;
        for (uint16_t col = x; col < x_end; col++) {
            *dst++ = color;
        }
    }
}

esp_err_t bsp_lcd_flush(void)
{
    if (s_fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_lcd_draw_bitmap(0, 0, BSP_LCD_WIDTH, BSP_LCD_HEIGHT, s_fb);
}

esp_err_t bsp_lcd_flush_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (s_fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x >= BSP_LCD_WIDTH || y >= BSP_LCD_HEIGHT || w == 0U || h == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t x_end = (uint16_t)BSP_MIN((uint32_t)x + w, (uint32_t)BSP_LCD_WIDTH);
    const uint16_t y_end = (uint16_t)BSP_MIN((uint32_t)y + h, (uint32_t)BSP_LCD_HEIGHT);

    for (uint16_t row = y; row < y_end; row++) {
        const uint16_t *src = s_fb + (size_t)row * BSP_LCD_WIDTH + x;
        esp_err_t err = bsp_lcd_draw_bitmap(x, row, x_end, (uint16_t)(row + 1U), src);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

/* ---------------- 直接绘制 ---------------- */

esp_err_t bsp_lcd_draw_bitmap(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                              const uint16_t *pixels)
{
    if (s_panel == NULL || pixels == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x0 >= x1 || y0 >= y1 || x1 > BSP_LCD_WIDTH || y1 > BSP_LCD_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 提交前清掉残留完成信号，保证后面等的是"这一次"传输 */
    (void)xSemaphoreTake(s_flush_sem, 0);

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, pixels);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 阻塞到 DMA 真正搬完：返回后调用方可以安全复用/释放 pixels */
    return bsp_lcd_wait_flush_done(BSP_FLUSH_TIMEOUT_MS);
}

esp_err_t bsp_lcd_draw_bitmap_async(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                                    const uint16_t *pixels)
{
    if (s_panel == NULL || pixels == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x0 >= x1 || y0 >= y1 || x1 > BSP_LCD_WIDTH || y1 > BSP_LCD_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 清掉残留完成信号，避免上一笔的回调被误当成本笔的 */
    (void)xSemaphoreTake(s_flush_sem, 0);

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, pixels);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap_async failed: %s", esp_err_to_name(err));
    }
    return err;
}

void bsp_lcd_set_flush_done_callback(bsp_lcd_flush_done_cb_t cb)
{
    s_done_cb = cb;
}

esp_err_t bsp_lcd_fill(uint16_t color)
{
    if (s_fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    bsp_lcd_fb_fill_rect(0, 0, BSP_LCD_WIDTH, BSP_LCD_HEIGHT, color);
    return bsp_lcd_flush();
}

esp_err_t bsp_lcd_wait_flush_done(uint32_t timeout_ms)
{
    if (s_flush_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "wait flush done timeout(%u ms)", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* ---------------- 底层与诊断 ---------------- */

esp_lcd_panel_handle_t bsp_lcd_panel(void)
{
    return s_panel;
}

esp_lcd_panel_io_handle_t bsp_lcd_panel_io(void)
{
    return s_io;
}

/* ---------------- 文字绘制 ---------------- */

/**
 * @brief  往帧缓冲写一个像素（越界自动丢弃）
 */
static inline void lcd_plot(int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= BSP_LCD_WIDTH || y >= BSP_LCD_HEIGHT) {
        return;
    }
    s_fb[(size_t)y * BSP_LCD_WIDTH + (size_t)x] = color;
}

/**
 * @brief  从 UTF-8 串解出一个码点并前移指针
 * @note   只在字符串内部读后续字节，遇到截断序列直接判为非法字符，
 *         不会越界读到 NUL 之后。
 */
static uint32_t lcd_utf8_next(const char **cursor)
{
    const uint8_t *s = (const uint8_t *)*cursor;
    uint32_t cp;

    if (s[0] < 0x80U) {
        *cursor += 1;
        return s[0];
    }
    if ((s[0] & 0xE0U) == 0xC0U && s[1] != 0U) {
        cp = ((uint32_t)(s[0] & 0x1FU) << 6) | (uint32_t)(s[1] & 0x3FU);
        *cursor += 2;
        return cp;
    }
    if ((s[0] & 0xF0U) == 0xE0U && s[1] != 0U && s[2] != 0U) {
        cp = ((uint32_t)(s[0] & 0x0FU) << 12) | ((uint32_t)(s[1] & 0x3FU) << 6) |
             (uint32_t)(s[2] & 0x3FU);
        *cursor += 3;
        return cp;
    }
    if ((s[0] & 0xF8U) == 0xF0U && s[1] != 0U && s[2] != 0U && s[3] != 0U) {
        cp = ((uint32_t)(s[0] & 0x07U) << 18) | ((uint32_t)(s[1] & 0x3FU) << 12) |
             ((uint32_t)(s[2] & 0x3FU) << 6) | (uint32_t)(s[3] & 0x3FU);
        *cursor += 4;
        return cp;
    }

    *cursor += 1;
    return 0xFFFDU;     /* 非法序列：按替换字符处理 */
}

int bsp_lcd_text_height(void)
{
    return BSP_UI_FONT_HEIGHT;
}

int bsp_lcd_text_width(const char *utf8)
{
    if (utf8 == NULL) {
        return 0;
    }

    int width = 0;
    const char *p = utf8;

    while (*p != '\0') {
        const bsp_ui_glyph_t *g = bsp_ui_font_lookup(lcd_utf8_next(&p));

        width += (g != NULL) ? (int)g->width : BSP_UI_ASCII_WIDTH;
    }
    return width;
}

void bsp_lcd_text(int x, int y, const char *utf8, uint16_t fg, uint16_t bg)
{
    if (s_fb == NULL || utf8 == NULL) {
        return;
    }

    int pen = x;
    const char *p = utf8;

    while (*p != '\0') {
        const bsp_ui_glyph_t *g = bsp_ui_font_lookup(lcd_utf8_next(&p));

        if (g == NULL) {
            /* 缺字：画一个实心方块，提示"这个字没进字库" */
            for (int gy = 0; gy < BSP_UI_FONT_HEIGHT; gy++) {
                for (int gx = 0; gx < BSP_UI_ASCII_WIDTH; gx++) {
                    lcd_plot(pen + gx, y + gy, fg);
                }
            }
            pen += BSP_UI_ASCII_WIDTH;
            continue;
        }

        /* 先铺字格底色（不透明，局部刷新时不残留旧字） */
        for (int gy = 0; gy < g->height; gy++) {
            for (int gx = 0; gx < g->width; gx++) {
                lcd_plot(pen + gx, y + gy, bg);
            }
        }

        /* 再按点阵置前景 */
        for (int gy = 0; gy < g->height; gy++) {
            const uint8_t *row = g->bits + (size_t)gy * g->stride;

            for (int gx = 0; gx < g->width; gx++) {
                if ((row[gx >> 3] & (uint8_t)(0x80U >> (gx & 7))) != 0U) {
                    lcd_plot(pen + gx, y + gy, fg);
                }
            }
        }

        pen += g->width;
    }
}

void bsp_lcd_text_right(int right_x, int y, const char *utf8, uint16_t fg, uint16_t bg)
{
    bsp_lcd_text(right_x - bsp_lcd_text_width(utf8), y, utf8, fg, bg);
}

int bsp_lcd_textf(int x, int y, uint16_t fg, uint16_t bg, const char *fmt, ...)
{
    char buf[128] = {0};

    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    bsp_lcd_text(x, y, buf, fg, bg);
    return bsp_lcd_text_width(buf);
}

void bsp_lcd_frame(int x, int y, int w, int h, uint16_t color)
{
    if (s_fb == NULL || w <= 0 || h <= 0) {
        return;
    }

    for (int i = 0; i < w; i++) {
        lcd_plot(x + i, y, color);
        lcd_plot(x + i, y + h - 1, color);
    }
    for (int i = 0; i < h; i++) {
        lcd_plot(x, y + i, color);
        lcd_plot(x + w - 1, y + i, color);
    }
}

/* ---------------- 内部实现 ---------------- */

/**
 * @brief  配置 RD 引脚为输出并拉高
 */
static void lcd_config_rd_pin(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = BSP_BIT(BSP_LCD_PIN_RD),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    gpio_config(&cfg);
    gpio_set_level(BSP_LCD_PIN_RD, 1);
}

/**
 * @brief  建立 8080 总线、面板 IO，并注册 ST7789 面板
 */
static esp_err_t lcd_new_bus_and_panel(void)
{
    /* max_transfer_bytes 取整屏大小：驱动按此尺寸准备搬运链路，
     * 保证"一次整屏刷新"不会被拆成异常的小块 */
    const esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src            = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num        = BSP_LCD_PIN_DC,
        .wr_gpio_num        = BSP_LCD_PIN_WR,
        .data_gpio_nums     = {
            BSP_LCD_PIN_D0, BSP_LCD_PIN_D1, BSP_LCD_PIN_D2, BSP_LCD_PIN_D3,
            BSP_LCD_PIN_D4, BSP_LCD_PIN_D5, BSP_LCD_PIN_D6, BSP_LCD_PIN_D7,
        },
        .bus_width          = 8,
        .max_transfer_bytes = (size_t)BSP_LCD_WIDTH * BSP_LCD_HEIGHT * sizeof(uint16_t),
        .psram_trans_align  = 64,
        .sram_trans_align   = 4,
    };
    esp_err_t err = esp_lcd_new_i80_bus(&bus_cfg, &s_i80_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new i80 bus failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num        = BSP_LCD_PIN_CS,
        .pclk_hz            = BSP_LCD_PCLK_HZ,
        .trans_queue_depth  = LCD_TRANS_QUEUE_DEPTH,
        .dc_levels          = {
            .dc_idle_level  = 0,
            .dc_cmd_level   = 0,
            .dc_dummy_level = 0,
            .dc_data_level  = 1,
        },
        .lcd_cmd_bits       = 8,
        .lcd_param_bits     = 8,
        .flags.swap_color_bytes = 1,            /* 8bit 总线一次写半像素，必须换字节序 */
        .on_color_trans_done = lcd_color_trans_done_cb,
    };

    err = esp_lcd_new_panel_io_i80(s_i80_bus, &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new panel io failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };

    err = esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new st7789 panel failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/**
 * @brief  切换为横屏（320x240）
 */
static void lcd_apply_landscape(void)
{
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
}
