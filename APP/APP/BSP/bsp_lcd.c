/* ================================================================
 * bsp_lcd —— LCD 驱动：SPI 初始化/清屏/区域刷新
 *
 * 架构位置：APP BSP 层；lcd_ui 绘制依赖
 * ================================================================ */
#include "bsp_lcd.h"
#include "LCD/lcd.h"
#include "logger.h"

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

/* 仅设置写窗口（CASET/PASET + RAM 写就绪），供 DMA 异步 flush 使用 */
void BSP_LCD_SetWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    lcd_set_window(x, y, w, h);
    lcd_write_ram_prepare();
}

/* LCD GRAM 写地址（供 DMA 传输目标） */
uint32_t BSP_LCD_GetRamAddr(void)
{
    return (uint32_t)&LCD->LCD_RAM;
}

/* 批量写入矩形像素（逐行"单行多列窗口"写：
 * ST7789 实测：单点窗口只收 1 像素（超窗丢弃）；多行窗口 RAMWR
 * 行递增异常（第二行起错位）。唯一正确路径 = 每行设 (x, y+row, w, 1)
 * 窗口 + 行数据连续写（rowmap 实测 240x1 全对）。 */
void BSP_LCD_WritePixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const uint16_t *buf)
{
    for (uint16_t row = 0; row < h; row++) {
        lcd_set_window(x, (uint16_t)(y + row), w, 1);
        lcd_write_ram_prepare();
        volatile uint16_t *ram = &LCD->LCD_RAM;
        const uint16_t *p = buf + (uint32_t)row * w;
        uint32_t n = w;
        while (n--) {
            *ram = *p++;
        }
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

/* 读回自检：定位部分刷新错位（"斜"/镜像花屏）的硬件级诊断。
 * 无大缓冲（主 SRAM 紧张）：渐变逐行即时生成写，蓝块循环写常量。 */
void BSP_LCD_SelfTest(void)
{
    uint16_t W = BSP_LCD_GetWidth();
    uint16_t H = BSP_LCD_GetHeight();
    LOG_Printf("LCD selftest: %ux%u id=0x%04X\r\n", W, H, BSP_LCD_GetId());

    /* 全屏水平渐变：每行 color = (x & 0x1F) << 11，逐行即时生成 */
    uint16_t grad[64];   /* 复用小块：每行分段写（64px/段） */
    for (uint16_t y = 0; y < H; y++) {
        for (uint16_t x0 = 0; x0 < W; x0 += 64) {
            uint16_t n = (uint16_t)((W - x0) > 64 ? 64 : (W - x0));
            for (uint16_t k = 0; k < n; k++) {
                grad[k] = (uint16_t)(((x0 + k) & 0x1Fu) << 11);
            }
            BSP_LCD_WritePixels(x0, y, n, 1, grad);
        }
    }
    /* 读回 5 点：预期 (0,0)=0x0000 (W-1,0)=0xF800 (0,H-1)=0x0000 (W-1,H-1)=0xF800 (W/2,H/2)=0x7800 */
    uint16_t p[5];
    uint16_t xs[5] = {0, (uint16_t)(W - 1), 0, (uint16_t)(W - 1), (uint16_t)(W / 2)};
    uint16_t ys[5] = {0, 0, (uint16_t)(H - 1), (uint16_t)(H - 1), (uint16_t)(H / 2)};
    uint16_t ex[5] = {0x0000, 0xF800, 0x0000, 0xF800, 0x7800};
    for (int i = 0; i < 5; i++) {
        p[i] = lcd_read_point_rgb565(xs[i], ys[i]);
        LOG_Printf("  pt(%u,%u)=0x%04X exp=0x%04X %s\r\n",
                   xs[i], ys[i], p[i], ex[i],
                   p[i] == ex[i] ? "OK" : "MISMATCH");
    }

    /* 部分窗口：60x30 蓝色块 @(90,150)，走 BSP_LCD_WritePixels 新路径 → 读回 4 角 */
    {
        uint16_t blue[60];   /* 复用单行缓冲：BSP 逐行写内部按行取 */
        for (int i = 0; i < 60; i++) blue[i] = 0x001F;
        /* 构造 30 行数据：用逐行 API 直接写（每行同缓冲） */
        for (int r = 0; r < 30; r++) {
            BSP_LCD_WritePixels(90, (uint16_t)(150 + r), 60, 1, blue);
        }
    }
    uint16_t b1 = lcd_read_point_rgb565(90, 150);
    uint16_t b2 = lcd_read_point_rgb565(149, 150);
    uint16_t b3 = lcd_read_point_rgb565(90, 179);
    uint16_t b4 = lcd_read_point_rgb565(149, 179);
    LOG_Printf("  win(90,150,60,30) corners: %04X %04X %04X %04X (exp all 0x001F)\r\n",
               b1, b2, b3, b4);

    /* 单点写读（无行宽依赖）：写 5 个孤立点立即读回，区分写错/读错 */
    const uint16_t sx[5] = {10, 100, 200, 150, 230};
    const uint16_t syy[5] = {20, 40, 60, 250, 300};
    const uint16_t sv[5] = {0x1234, 0xABCD, 0x0F0F, 0x55AA, 0xF00D};
    for (int i = 0; i < 5; i++) {
        lcd_set_cursor(sx[i], syy[i]);
        lcd_write_ram_prepare();
        LCD->LCD_RAM = sv[i];
        uint16_t rv = lcd_read_point_rgb565(sx[i], syy[i]);
        LOG_Printf("  dot(%u,%u) w=0x%04X r=0x%04X %s\r\n",
                   sx[i], syy[i], sv[i], rv, rv == sv[i] ? "OK" : "MISMATCH");
    }

    /* 小窗口映射测试：写递增像素序列，读回各列，建立列映射 */
    lcd_clear(BLACK);
    {
        /* 窗口 (0,0,240,1)：写 pixel[i]=i 编码（i&0x1F）<<11 */
        uint16_t row[240];
        for (int i = 0; i < 240; i++) {
            row[i] = (uint16_t)((i & 0x1Fu) << 11);
        }
        lcd_set_window(0, 0, 240, 1);
        lcd_write_ram_prepare();
        volatile uint16_t *ram = &LCD->LCD_RAM;
        for (int i = 0; i < 240; i++) {
            *ram = row[i];
        }
        const int cs[6] = {0, 60, 120, 180, 239, 119};
        LOG_Printf("  rowmap(240x1):");
        for (int k = 0; k < 6; k++) {
            uint16_t rv = lcd_read_point_rgb565((uint16_t)cs[k], 0);
            LOG_Printf(" c%d=0x%04X", cs[k], rv);
        }
        LOG_Printf("\r\n");
    }
    {
        /* 窗口 (50,100,100,1)：写 pixel[i]=0x8000|i */
        uint16_t row[100];
        for (int i = 0; i < 100; i++) {
            row[i] = (uint16_t)(0x8000u | (uint16_t)i);
        }
        lcd_set_window(50, 100, 100, 1);
        lcd_write_ram_prepare();
        volatile uint16_t *ram = &LCD->LCD_RAM;
        for (int i = 0; i < 100; i++) {
            *ram = row[i];
        }
        const int cs2[5] = {50, 99, 149, 70, 140};
        LOG_Printf("  rowmap(100x1@50):");
        for (int k = 0; k < 5; k++) {
            uint16_t rv = lcd_read_point_rgb565((uint16_t)cs2[k], 100);
            LOG_Printf(" c%d=0x%04X", cs2[k], rv);
        }
        LOG_Printf("\r\n");
    }

    /* 密集网格：3 行不同色（红/绿/蓝），读 3x3 网格定位行错位 */
    {
        uint16_t red[60], grn[60], blu[60];
        for (int i = 0; i < 60; i++) {
            red[i] = 0xF800; grn[i] = 0x07E0; blu[i] = 0x001F;
        }
        BSP_LCD_WritePixels(90, 150, 60, 1, red);
        BSP_LCD_WritePixels(90, 151, 60, 1, grn);
        BSP_LCD_WritePixels(90, 152, 60, 1, blu);
        for (int r = 0; r < 3; r++) {
            uint16_t a = lcd_read_point_rgb565(90, (uint16_t)(150 + r));
            uint16_t b = lcd_read_point_rgb565(120, (uint16_t)(150 + r));
            uint16_t c = lcd_read_point_rgb565(149, (uint16_t)(150 + r));
            LOG_Printf("  grid y=%u: %04X %04X %04X (exp %04X)\r\n",
                       (unsigned)(150 + r), a, b, c,
                       (r == 0) ? 0xF800u : (r == 1) ? 0x07E0u : 0x001Fu);
        }
    }

    /* 清屏还原 */
    lcd_clear(BLACK);
    LOG_Printf("LCD selftest done (screen cleared)\r\n");
}
