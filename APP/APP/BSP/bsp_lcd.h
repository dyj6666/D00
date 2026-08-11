/* ================================================================
 * bsp_lcd —— LCD 屏驱动：初始化/清屏/区域刷新
 *
 * 架构位置：APP BSP 层；lcd_ui 状态页绘制依赖本层
 * ================================================================ */
#ifndef BSP_LCD_H
#define BSP_LCD_H

#include <stdint.h>

/* ================================================================
 * 板载 2.8 寸 TFTLCD BSP 层（统一接口）
 *
 * 底层：官方多 IC 驱动（BSP/LCD/lcd.c，自动识别 ST7789/ILI9341 等），
 *       FSMC BANK4 并口，本层封装为 bsp_ 风格统一 API。
 * 性能：字符窗口连续写（6 倍提速）、清屏循环展开、写时序 12ns。
 * ================================================================ */

/* ---------- 常用 16 位 RGB565 颜色 ---------- */
#define BSP_LCD_COLOR_BLACK     0x0000
#define BSP_LCD_COLOR_WHITE     0xFFFF
#define BSP_LCD_COLOR_RED       0xF800
#define BSP_LCD_COLOR_GREEN     0x07E0
#define BSP_LCD_COLOR_BLUE      0x001F
#define BSP_LCD_COLOR_YELLOW    0xFFE0
#define BSP_LCD_COLOR_CYAN      0x07FF
#define BSP_LCD_COLOR_MAGENTA   0xF81F
#define BSP_LCD_COLOR_GRAY      0x7BEF
#define BSP_LCD_COLOR_LGRAY     0xC618
#define BSP_LCD_COLOR_ORANGE    0xFD20
#define BSP_LCD_COLOR_NAVY      0x000F

/* ---------- 显示方向 ---------- */
typedef enum {
    BSP_LCD_ORIENT_PORTRAIT = 0,   /* 竖屏 240x320 */
    BSP_LCD_ORIENT_LANDSCAPE,      /* 横屏 320x240 */
} bsp_lcd_orient_t;

/* ---------- 字符大小 ---------- */
typedef enum {
    BSP_LCD_FONT_12 = 12,   /* 6x12 */
    BSP_LCD_FONT_16 = 16,   /* 8x16 */
    BSP_LCD_FONT_24 = 24,   /* 12x24 */
} bsp_lcd_font_t;

/* ---------- 接口 ---------- */

/* 初始化：自动识别控制器（ST7789/ILI9341 等），默认竖屏 + 背光开 + 清屏。
 * 返回控制器 ID（0x7789=ST7789 等）。 */
uint16_t BSP_LCD_Init(void);

/* 查询控制器 ID / 逻辑宽高 */
uint16_t BSP_LCD_GetId(void);
uint16_t BSP_LCD_GetWidth(void);
uint16_t BSP_LCD_GetHeight(void);

/* 显示方向：竖屏/横屏 */
void BSP_LCD_SetOrient(bsp_lcd_orient_t orient);

/* 直接设置扫描方向（0-7，官方 8 方向，用于诊断/特殊适配） */
void BSP_LCD_ScanDir(uint8_t dir);

/* 全屏清屏 / 区域填充（任意矩形） */
void BSP_LCD_Clear(uint16_t color);
void BSP_LCD_Fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                  uint16_t color);

/* 批量像素读写（光标覆盖层/图像等精确恢复用） */
void BSP_LCD_ReadPixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        uint16_t *buf);
void BSP_LCD_WritePixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const uint16_t *buf);

/* 画点 / 线 / 矩形框 / 圆 */
void BSP_LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color);
void BSP_LCD_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                      uint16_t color);
void BSP_LCD_DrawRect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                      uint16_t color);
void BSP_LCD_DrawCircle(uint16_t cx, uint16_t cy, uint16_t r, uint16_t color);

/* 字符 / 字符串（color 前景，背景用 BSP_LCD_COLOR_BLACK 自动填充） */
uint16_t BSP_LCD_ShowChar(uint16_t x, uint16_t y, char c,
                          uint16_t color, bsp_lcd_font_t font);
void BSP_LCD_ShowString(uint16_t x, uint16_t y, const char *s,
                        uint16_t color, bsp_lcd_font_t font);
void BSP_LCD_ShowNum(uint16_t x, uint16_t y, uint32_t value, uint8_t digits,
                     uint16_t color, bsp_lcd_font_t font);
void BSP_LCD_ShowHex(uint16_t x, uint16_t y, uint32_t value, uint8_t digits,
                     uint16_t color, bsp_lcd_font_t font);

/* 背光控制（1 亮 / 0 灭） */
void BSP_LCD_Backlight(uint8_t on);

/* 性能基准（DWT 计时，输出清屏/填充/字符/字符串速率；结束自动清屏） */
void BSP_LCD_Bench(void);

#endif
