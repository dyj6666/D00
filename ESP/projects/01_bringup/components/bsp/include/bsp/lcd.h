/* ================================================================
 * lcd.h —— 2.4 寸 320x240 TFT（8080 8bit 并口 + ST7789VW）
 *
 * 上屏模型（性能核心）：
 *   板载屏幕是 8080 并口，一次像素传输 = 一次 DMA。BSP 提供"整帧帧缓冲"
 *   （320*240*2 = 150KB，放 PSRAM），应用只管往缓冲里画，最后 flush 一次。
 *   这样只有一笔 DMA、且传输期间数据不会被改写，既不撕裂也不浪费 CPU。
 *
 * 关于"提交即返回"的重要约定：
 *   i80 面板 IO 的 draw_bitmap 是把缓冲区指针交给 DMA 后立刻返回的
 *   （见本工程 ENGINEERING_LOG.md 第 1 条）。因此：
 *     - 用 bsp_lcd_flush()/bsp_lcd_flush_rect() 最安全（内部会等 DMA 完成）；
 *     - 直接用 bsp_lcd_draw_bitmap() 时，函数返回时传输已结束（内部同样等待），
 *       所以调用方可以立刻复用/释放缓冲区。
 *
 * 背光不在 GPIO 上，挂在 XL9555 的 P0_7，只能开/关（不支持 PWM 调光）。
 * ================================================================ */
#ifndef __BSP_LCD_H
#define __BSP_LCD_H

#include "bsp_types.h"
#include "bsp_pinmap.h"      /* BSP_LCD_WIDTH / BSP_LCD_HEIGHT / 各 LCD 引脚 */

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 常用 RGB565 颜色 ---------------- */

#define BSP_LCD_COLOR_BLACK     0x0000U
#define BSP_LCD_COLOR_WHITE     0xFFFFU
#define BSP_LCD_COLOR_RED       0xF800U
#define BSP_LCD_COLOR_GREEN     0x07E0U
#define BSP_LCD_COLOR_BLUE      0x001FU
#define BSP_LCD_COLOR_YELLOW    0xFFE0U
#define BSP_LCD_COLOR_CYAN      0x07FFU
#define BSP_LCD_COLOR_MAGENTA   0xF81FU
#define BSP_LCD_COLOR_ORANGE    0xFD20U
#define BSP_LCD_COLOR_GRAY      0x8410U
#define BSP_LCD_COLOR_DARKBLUE  0x0214U     /* RGB(0,64,160)，名字卡前景色 */

/** 24bit RGB -> RGB565（编译期可算，写配色时少出错） */
#define BSP_RGB565(r, g, b)     ((uint16_t)((((r) & 0xF8U) << 8) | (((g) & 0xFCU) << 3) | ((b) >> 3)))

/* ---------------- 生命周期 ---------------- */

/**
 * @brief  初始化 LCD：XL9555 背光 -> 8080 总线 -> ST7789 -> 点亮背光
 * @note   依赖 I2C + XL9555（bsp_init 会自动补齐）
 */
esp_err_t bsp_lcd_init(void);

/**
 * @brief  关闭背光并释放面板与总线
 */
esp_err_t bsp_lcd_deinit(void);

/* ---------------- 背光 ---------------- */

/**
 * @brief  开关背光
 */
esp_err_t bsp_lcd_set_backlight(bool on);

/**
 * @brief  查询背光状态
 */
bool bsp_lcd_backlight_is_on(void);

/* ---------------- 帧缓冲（推荐用法）---------------- */

/**
 * @brief  获取整帧帧缓冲（RGB565、行优先、320x240，PSRAM，常驻不释放）
 * @return 缓冲区首地址；未初始化返回 NULL
 *
 * @note   宽高与跨度：行跨度 = BSP_LCD_WIDTH 像素。
 *         应用画完后调用 bsp_lcd_flush() 整体上屏。
 */
uint16_t *bsp_lcd_fb(void);

/**
 * @brief  把帧缓冲里的指定矩形区域画成单色（CPU 侧操作，不碰总线）
 */
void bsp_lcd_fb_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
 * @brief  把整块帧缓冲一次推上屏（阻塞到 DMA 完成）
 */
esp_err_t bsp_lcd_flush(void);

/**
 * @brief  把帧缓冲中的一块矩形区域推上屏（阻塞到 DMA 完成）
 * @param  x,y,w,h 屏幕坐标与尺寸（像素）
 * @note   局部刷新按行发起，每行一次传输并等待完成，适合只改一行的场景。
 */
esp_err_t bsp_lcd_flush_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/* ---------------- 直接绘制（不经过帧缓冲）---------------- */

/**
 * @brief  直接推送一块 RGB565 位图（阻塞到 DMA 完成，返回后即可复用缓冲）
 * @param  x0,y0 左上角（含）
 * @param  x1,y1 右下角（不含）
 * @param  pixels 像素数据，长度 >= (x1-x0)*(y1-y0)
 */
esp_err_t bsp_lcd_draw_bitmap(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                              const uint16_t *pixels);

/**
 * @brief  整屏刷单色（内部走帧缓冲，性能最好）
 */
esp_err_t bsp_lcd_fill(uint16_t color);

/* ---------------- 文字绘制（板载点阵字库，中英文混排）---------------- */

/**
 * @brief  字库行高（像素），固定 16
 */
int bsp_lcd_text_height(void);

/**
 * @brief  计算一段 UTF-8 文本的像素宽度（ASCII 8px，汉字 16px）
 */
int bsp_lcd_text_width(const char *utf8);

/**
 * @brief  在帧缓冲上画一段 UTF-8 文本
 * @param  x,y  左上角（帧缓冲坐标）
 * @param  fg   前景色
 * @param  bg   背景色（每个字格都会先填成 bg，所以文字不透明、不会闪）
 * @note   只改帧缓冲，不自动上屏；画完记得 bsp_lcd_flush() / bsp_lcd_flush_rect()。
 *         未收录的字符会画成一个实心方块——缺字一眼可见，而不是静默不显示。
 */
void bsp_lcd_text(int x, int y, const char *utf8, uint16_t fg, uint16_t bg);

/**
 * @brief  右对齐画文本（right_x 为文本右边界）
 */
void bsp_lcd_text_right(int right_x, int y, const char *utf8, uint16_t fg, uint16_t bg);

/**
 * @brief  格式化后画文本（printf 风格，内部缓冲 128 字节）
 * @return 实际绘制宽度（像素）
 */
int bsp_lcd_textf(int x, int y, uint16_t fg, uint16_t bg, const char *fmt, ...);

/**
 * @brief  画 1px 空心矩形边框（帧缓冲上）
 */
void bsp_lcd_frame(int x, int y, int w, int h, uint16_t color);

/* ---------------- 底层与诊断 ---------------- */

/**
 * @brief  获取面板句柄（需要自定义 esp_lcd 操作时使用）
 */
esp_lcd_panel_handle_t bsp_lcd_panel(void);

/**
 * @brief  获取面板 IO 句柄
 */
esp_lcd_panel_io_handle_t bsp_lcd_panel_io(void);

/**
 * @brief  等待最近一次提交的像素真正搬完
 */
esp_err_t bsp_lcd_wait_flush_done(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_LCD_H */
