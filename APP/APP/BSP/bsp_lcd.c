/* ================================================================
 * bsp_lcd —— LCD 驱动：SPI 初始化/清屏/区域刷新
 *
 * 架构位置：APP BSP 层；lcd_ui 绘制依赖
 * ================================================================ */
#include "bsp_lcd.h"
#include "LCD/lcd.h"
#include "logger.h"
#include "mem_map.h"

#include <string.h>

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
    /* 上电快速自检：写读链路验证（防花屏守护层 1），失败重初始化一次 */
    if (!BSP_LCD_QuickSelfTest()) {
        LOG_Printf("[LCD] quick self-test failed, re-init...\r\n");
        lcd_init();
        lcd_display_dir(BSP_LCD_ORIENT_PORTRAIT);
        LCD_BL(1);
        lcd_clear(BSP_LCD_COLOR_BLACK);
        (void)BSP_LCD_QuickSelfTest();
    }
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

/* ---------------- 防花屏守护层 ----------------
 * 三层保护（上电自检 / flush 运行时抽检 / 完整自检命令），任何一层
 * 检测到 GRAM 写读不一致即计数+日志，杜绝花屏"静默"上线。 */
static uint32_t s_spot_fails;
static uint32_t s_spot_checks;

uint32_t BSP_LCD_GetSpotCheckFails(void) { return s_spot_fails; }
uint32_t BSP_LCD_GetSpotCheckCount(void) { return s_spot_checks; }

/* 上电快速自检：3 行不同色 + 3 单点，写读回全对才算通过 */
bool BSP_LCD_QuickSelfTest(void)
{
    uint16_t W = BSP_LCD_GetWidth();
    uint16_t H = BSP_LCD_GetHeight();
    bool ok = true;

    /* 3 行不同色（用屏幕中部），验证单行窗口写路径 */
    const uint16_t colors[3] = {0xF800, 0x07E0, 0x001F};
    for (int r = 0; r < 3; r++) {
        uint16_t row[64];
        for (int i = 0; i < 64; i++) row[i] = colors[r];
        BSP_LCD_WritePixels((uint16_t)(W / 2 - 32), (uint16_t)(H / 2 - 2 + r), 64, 1, row);
        const uint16_t cols[3] = {0u, 32u, 63u};   /* 窗口内三列（63 为末列） */
        for (int c = 0; c < 3; c++) {
            uint16_t rv = lcd_read_point_rgb565((uint16_t)(W / 2 - 32 + cols[c]),
                                                (uint16_t)(H / 2 - 2 + r));
            if (rv != colors[r]) {
                ok = false;
                s_spot_fails++;
            }
        }
    }
    /* 3 单点（四角+中心），验证任意坐标写读 */
    const uint16_t px[3] = {2, (uint16_t)(W - 3), (uint16_t)(W / 2)};
    const uint16_t py[3] = {2, (uint16_t)(H - 3), (uint16_t)(H / 2)};
    const uint16_t pv[3] = {0x1234, 0xABCD, 0x55AA};
    for (int i = 0; i < 3; i++) {
        lcd_set_cursor(px[i], py[i]);
        lcd_write_ram_prepare();
        LCD->LCD_RAM = pv[i];
        if (lcd_read_point_rgb565(px[i], py[i]) != pv[i]) {
            ok = false;
            s_spot_fails++;
        }
    }
    if (!ok) {
        LOG_Printf("[LCD] SELF-TEST FAILED! (%lu fails)\r\n",
                   (unsigned long)s_spot_fails);
    }
    return ok;
}

/* 扫描方向诊断：遍历 8 种 MADCTL 方向测多行窗口连续写（读写同坐标对称测试） */
void BSP_LCD_DirTest(void)
{
    static const uint16_t colors[3] = {0xF800, 0x07E0, 0x001F};
    uint8_t saved = DFT_SCAN_DIR;
    for (uint8_t d = 0; d < 8; d++) {
        lcd_scan_dir(d);
        lcd_clear(BLACK);
        /* 多行窗口 (30,40,50,3)：连续写 150 像素（每行 50） */
        lcd_set_window(30, 40, 50, 3);
        lcd_write_ram_prepare();
        volatile uint16_t *ram = &LCD->LCD_RAM;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 50; c++) {
                *ram = colors[r];
            }
        }
        bool ok = true;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                uint16_t rv = lcd_read_point_rgb565((uint16_t)(30 + c * 25),
                                                    (uint16_t)(40 + r));
                if (rv != colors[r]) {
                    ok = false;
                }
            }
        }
        LOG_Printf("[LCD] dir=%u multirow-write: %s\r\n", (unsigned)d,
                   ok ? "OK" : "FAIL");
    }
    lcd_scan_dir(saved);   /* 恢复原方向 */
    lcd_clear(BLACK);
    LOG_Printf("[LCD] dirtest done (dir restored=%u)\r\n", (unsigned)saved);
}

/* DMA M2M 直写（F407 正确配置）：
 * 关键配置（DAP 寄存器级实验确证，见 ENGINEERING_LOG 10.57）：
 *   DIR 位为 2 比特编码：00=P2M、01=M2P、10=M2M——M2M 必须 DIR_1；
 *   M2M 数据流：PAR=源（PINC 递增）、M0AR=目的（MINC 固定）；
 *   与 CPU 写路径完全同构（逐行单行窗口 + 行数据）。 */
bool BSP_LCD_WritePixelsDma(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            const uint16_t *buf)
{
    if (w == 0u || h == 0u || buf == NULL) {
        return false;
    }
    if ((uint32_t)x + w > BSP_LCD_GetWidth() ||
        (uint32_t)y + h > BSP_LCD_GetHeight()) {
        return false;
    }
    __HAL_RCC_DMA2_CLK_ENABLE();

    for (uint16_t row = 0; row < h; row++) {
        if (row == 0u) {
            lcd_set_window(x, y, w, 1);
        } else {
            uint16_t yy = (uint16_t)(y + row);
            lcd_wr_regno(lcddev.setycmd);
            lcd_wr_data(yy >> 8);
            lcd_wr_data(yy & 0xFF);
            lcd_wr_data(yy >> 8);
            lcd_wr_data(yy & 0xFF);
        }
        lcd_write_ram_prepare();

        DMA2_Stream4->CR = 0u;
        DMA2->HIFCR = DMA_HIFCR_CTCIF4;
        DMA2_Stream4->PAR  = (uint32_t)(buf + (uint32_t)row * w);  /* 源 */
        DMA2_Stream4->M0AR = BSP_LCD_GetRamAddr();                /* 目的 */
        DMA2_Stream4->NDTR = w;
        DMA2_Stream4->FCR  = DMA_SxFCR_DMDIS;
        DMA2_Stream4->CR = DMA_SxCR_CHSEL_0 |   /* M2M 通道位无意义 */
                           DMA_SxCR_PINC |      /* 源递增 */
                           DMA_SxCR_MSIZE_1 | DMA_SxCR_PSIZE_1 |  /* 16bit */
                           DMA_SxCR_PL_1 |
                           DMA_SxCR_DIR_1;      /* 10 = M2M（关键！） */
        DMA2_Stream4->CR |= DMA_SxCR_EN;        /* EN 最后单独置位 */
        uint32_t guard = 0;
        while (DMA2_Stream4->CR & DMA_SxCR_EN) {
            if (++guard > 40000000u) {          /* ~240ms 超时 */
                DMA2_Stream4->CR = 0u;
                return false;
            }
        }
        DMA2_Stream4->CR = 0u;
    }
    return true;
}

/* 批量写入矩形像素（逐行"单行多列窗口"写：
 * ST7789 实测：单点窗口只收 1 像素（超窗丢弃）；多行窗口 RAMWR
 * 行递增异常（第二行起错位）。唯一正确路径 = 每行设 (x, y+row, w, 1)
 * 窗口 + 行数据连续写（rowmap 实测 240x1 全对）。 */
void BSP_LCD_WritePixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const uint16_t *buf)
{
    if (w == 0u || h == 0u || buf == NULL) {
        return;
    }
    /* 边界防御：越界写直接拒绝（防窗口参数错误导致乱写 GRAM） */
    if ((uint32_t)x + w > BSP_LCD_GetWidth() ||
        (uint32_t)y + h > BSP_LCD_GetHeight()) {
        s_spot_fails++;
        LOG_Printf("[LCD] WritePixels OOB! (%u,%u,%u,%u)\r\n", x, y, w, h);
        return;
    }
    for (uint16_t row = 0; row < h; row++) {
        if (row == 0u) {
            /* 首行：完整窗口（CASET+PASET） */
            lcd_set_window(x, y, w, 1);
        } else {
            /* 后续行：仅增量更新 PASET 起点=终点（CASET 列区间保持），
             * 每次省 6 次 FSMC 寄存器写；ST7789 多行窗口硬件不可用
             * （dirtest 实测 8 方向全 FAIL），逐行是唯一正确路径。 */
            uint16_t yy = (uint16_t)(y + row);
            lcd_wr_regno(lcddev.setycmd);
            lcd_wr_data(yy >> 8);
            lcd_wr_data(yy & 0xFF);
            lcd_wr_data(yy >> 8);
            lcd_wr_data(yy & 0xFF);
        }
        lcd_write_ram_prepare();
        volatile uint16_t *ram = &LCD->LCD_RAM;
        const uint16_t *p = buf + (uint32_t)row * w;
        uint32_t n = w;
        while (n--) {
            *ram = *p++;
        }
    }
    /* 运行时抽检（防花屏守护）：每 128 次调用读回 1 点对比，
     * 平均开销 <0.1%；连续错误说明写路径被破坏（如误改连续写） */
    if (((++s_spot_checks) & 0x7Fu) == 0u) {
        uint16_t rv = lcd_read_point_rgb565(x, (uint16_t)(y + h - 1));
        uint16_t exp = buf[(uint32_t)(h - 1) * w];
        if (rv != exp) {
            s_spot_fails++;
            LOG_Printf("[LCD] SPOT CHECK FAIL: (%u,%u) w=0x%04X r=0x%04X (fails=%lu)\r\n",
                       x, (unsigned)(y + h - 1), exp, rv,
                       (unsigned long)s_spot_fails);
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
    /* 读回 5 点：预期 = (x&31)<<11（渐变编码），非固定值 */
    uint16_t p[5];
    uint16_t xs[5] = {0, (uint16_t)(W - 1), 0, (uint16_t)(W - 1), (uint16_t)(W / 2)};
    uint16_t ys[5] = {0, 0, (uint16_t)(H - 1), (uint16_t)(H - 1), (uint16_t)(H / 2)};
    uint16_t ex[5];
    for (int i = 0; i < 5; i++) {
        ex[i] = (uint16_t)((xs[i] & 0x1Fu) << 11);
    }
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
