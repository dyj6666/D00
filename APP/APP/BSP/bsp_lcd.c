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
}

void BSP_LCD_Fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                  uint16_t color)
{
    lcd_fill(x0, y0, x1, y1, color);
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
