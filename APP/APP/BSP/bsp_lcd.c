/* ================================================================
 * bsp_lcd —— LCD 驱动：SPI 初始化/清屏/区域刷新
 *
 * 架构位置：APP BSP 层；lcd_ui 绘制依赖
 * ================================================================ */
#include "bsp_lcd.h"
#include "LCD/lcd.h"

/* ================================================================
 * 板载 TFTLCD BSP 实现：封装官方多 IC 驱动（BSP/LCD/）为统一接口
 * ================================================================ */

uint16_t BSP_LCD_Init(void)
{
    g_back_color = BSP_LCD_COLOR_BLACK;
    lcd_init();
    lcd_display_dir(BSP_LCD_ORIENT_PORTRAIT);
    LCD_BL(1);
    lcd_clear(BSP_LCD_COLOR_BLACK);
    return lcddev.id;
}

uint16_t BSP_LCD_GetId(void)
{
    return lcddev.id;
}

uint16_t BSP_LCD_GetWidth(void)
{
    return lcddev.width;
}

uint16_t BSP_LCD_GetHeight(void)
{
    return lcddev.height;
}

void BSP_LCD_SetOrient(bsp_lcd_orient_t orient)
{
    lcd_display_dir((uint8_t)orient);
}

void BSP_LCD_ScanDir(uint8_t dir)
{
    lcd_scan_dir(dir);
}

void BSP_LCD_Clear(uint16_t color)
{
    lcd_clear(color);
    HAL_Delay(40);   /* 全屏清屏后等待面板刷新（~60Hz 两帧），防撕裂残留 */
}

void BSP_LCD_Fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                  uint16_t color)
{
    lcd_fill(x0, y0, x1, y1, color);
}

/* 批量读取矩形像素（逐点读回，光标覆盖层精确恢复用） */
void BSP_LCD_ReadPixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        uint16_t *buf)
{
    for (uint16_t j = 0; j < h; j++) {
        for (uint16_t i = 0; i < w; i++) {
            buf[(uint32_t)j * w + i] = lcd_read_point_rgb565(
                (uint16_t)(x + i), (uint16_t)(y + j));
        }
    }
}

/* 批量写入矩形像素（单窗口连续写，性能最优） */
void BSP_LCD_WritePixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const uint16_t *buf)
{
    lcd_set_window(x, y, w, h);
    lcd_write_ram_prepare();
    volatile uint16_t *ram = &LCD->LCD_RAM;
    uint32_t n = (uint32_t)w * h;
    for (uint32_t i = 0; i < n; i++) {
        *ram = buf[i];
    }
}

void BSP_LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color)
{
    lcd_draw_point(x, y, color);
}

void BSP_LCD_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                      uint16_t color)
{
    lcd_draw_line(x0, y0, x1, y1, color);
}

void BSP_LCD_DrawRect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                      uint16_t color)
{
    lcd_draw_rectangle(x0, y0, x1, y1, color);
}

void BSP_LCD_DrawCircle(uint16_t cx, uint16_t cy, uint16_t r, uint16_t color)
{
    lcd_draw_circle(cx, cy, (uint8_t)r, color);
}

uint16_t BSP_LCD_ShowChar(uint16_t x, uint16_t y, char c,
                          uint16_t color, bsp_lcd_font_t font)
{
    lcd_show_char(x, y, c, (uint8_t)font, 0, color);
    return (uint16_t)(x + (uint16_t)font / 2);
}

void BSP_LCD_ShowString(uint16_t x, uint16_t y, const char *s,
                        uint16_t color, bsp_lcd_font_t font)
{
    lcd_show_string(x, y, lcddev.width, lcddev.height,
                    (uint8_t)font, (char *)s, color);
}

void BSP_LCD_ShowNum(uint16_t x, uint16_t y, uint32_t value, uint8_t digits,
                     uint16_t color, bsp_lcd_font_t font)
{
    lcd_show_num(x, y, value, digits, (uint8_t)font, color);
}

void BSP_LCD_ShowHex(uint16_t x, uint16_t y, uint32_t value, uint8_t digits,
                     uint16_t color, bsp_lcd_font_t font)
{
    lcd_show_xnum(x, y, value, digits, (uint8_t)font, 16, color);
}

void BSP_LCD_Backlight(uint8_t on)
{
    LCD_BL(on);
}

void BSP_LCD_Bench(void)
{
    lcd_bench();
}
