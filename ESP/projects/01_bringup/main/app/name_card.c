/* ================================================================
 * name_card.c —— 开机名字卡（在 LCD 上显示汉字）
 *
 * 渲染方式：直接画进 BSP 提供的整帧帧缓冲，再一次性 bsp_lcd_flush()。
 *
 * 为什么这么写（踩过的坑都在这里）：
 *   i80 面板 IO 是"提交即返回 + 零拷贝"的，若一行一行地复用同一个行缓冲，
 *   下一行的写入会盖掉正在被 DMA 搬运的数据，屏幕上会出现内容缺失。
 *   BSP 的 lcd 模块已经把它收敛成"整帧缓冲 + 单次传输 + 等待完成"，
 *   应用侧只需要往缓冲里画，然后 flush 一次即可。
 * ================================================================ */
#include "name_card.h"

#include "bsp/lcd.h"
#include "font_name_64.h"

#include "esp_log.h"

static const char *TAG = "NAMECARD";

/* ---- 样式（改这里即可换配色；字模尺寸由 tools/gen_font.py 决定）---- */
#define NAME_CARD_BG        BSP_LCD_COLOR_WHITE       /* 背景：白 */
#define NAME_CARD_FG        BSP_LCD_COLOR_DARKBLUE    /* 前景：深蓝 */
#define NAME_GLYPH_GAP      10                        /* 字间距，须与 tools/gen_font.py 的 SPACING_PX 一致 */

/* 上屏自检框：沿屏幕四边画 1 像素边框，用来一眼确认整屏可达。
 * 不需要时改成 0 重新编译即可，不影响名字本身。 */
#define NAME_CARD_DIAG_FRAME    1

/** 8bit 覆盖率 -> RGB565 的混合色表（抗锯齿查表用） */
static uint16_t s_blend_lut[256];

/**
 * @brief  把 8bit 覆盖率展开成 RGB565 混合色表
 * @param  bg  背景色
 * @param  fg  前景色
 * @note   先把 5/6/5 分量补齐成 8bit 再线性插值，最后收回 5/6/5，
 *         避免直接对压好的 RGB565 整数做插值导致偏色。
 */
static void name_card_build_blend_lut(uint16_t bg, uint16_t fg)
{
    /* 提取并扩展到 8bit：r5->r8 用 (v<<3)|(v>>2)，g6->g8 用 (v<<2)|(v>>4) */
    const uint32_t br = ((bg >> 11) & 0x1F), bg6 = ((bg >> 5) & 0x3F), bb = (bg & 0x1F);
    const uint32_t fr = ((fg >> 11) & 0x1F), fg6 = ((fg >> 5) & 0x3F), fb = (fg & 0x1F);

    const uint32_t br8 = (br << 3) | (br >> 2);
    const uint32_t bg8 = (bg6 << 2) | (bg6 >> 4);
    const uint32_t bb8 = (bb << 3) | (bb >> 2);

    const uint32_t fr8 = (fr << 3) | (fr >> 2);
    const uint32_t fg8 = (fg6 << 2) | (fg6 >> 4);
    const uint32_t fb8 = (fb << 3) | (fb >> 2);

    for (int a = 0; a < 256; a++) {
        /* 线性插值 + 四舍五入（+127 后再除 255） */
        const uint32_t r = (fr8 * (uint32_t)a + br8 * (uint32_t)(255 - a) + 127U) / 255U;
        const uint32_t g = (fg8 * (uint32_t)a + bg8 * (uint32_t)(255 - a) + 127U) / 255U;
        const uint32_t b = (fb8 * (uint32_t)a + bb8 * (uint32_t)(255 - a) + 127U) / 255U;

        s_blend_lut[a] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

/**
 * @brief  把名字画进整帧帧缓冲
 * @param  fb BSP 帧缓冲（BSP_LCD_WIDTH x BSP_LCD_HEIGHT，行优先）
 */
static void name_card_render(uint16_t *fb)
{
    const int total_w = NAME_GLYPH_COUNT * NAME_GLYPH_W + (NAME_GLYPH_COUNT - 1) * NAME_GLYPH_GAP;
    const int x0 = (BSP_LCD_WIDTH  - total_w) / 2;              /* 水平居中 */
    const int y0 = (BSP_LCD_HEIGHT - NAME_GLYPH_H) / 2;         /* 垂直居中 */

    name_card_build_blend_lut(NAME_CARD_BG, NAME_CARD_FG);

    for (int y = 0; y < BSP_LCD_HEIGHT; y++) {
        uint16_t *row = fb + (size_t)y * BSP_LCD_WIDTH;
        const int gy = y - y0;                                  /* 本行在字形内的行号 */

        /* ① 先整行铺底色（字形外的行到这一步就完成了） */
        for (int x = 0; x < BSP_LCD_WIDTH; x++) {
            row[x] = NAME_CARD_BG;
        }

        /* ② 落在字形带内的行，用覆盖率查表覆盖对应像素 */
        if (gy >= 0 && gy < NAME_GLYPH_H) {
            for (int g = 0; g < NAME_GLYPH_COUNT; g++) {
                const uint8_t *alpha = name_glyphs[g].alpha + (size_t)gy * NAME_GLYPH_W;
                const int pen_x = x0 + g * (NAME_GLYPH_W + NAME_GLYPH_GAP);

                for (int dx = 0; dx < NAME_GLYPH_W; dx++) {
                    row[pen_x + dx] = s_blend_lut[alpha[dx]];
                }
            }
        }
    }

#if NAME_CARD_DIAG_FRAME
    for (int x = 0; x < BSP_LCD_WIDTH; x++) {
        fb[x] = NAME_CARD_FG;                                                   /* 上边 */
        fb[(size_t)(BSP_LCD_HEIGHT - 1) * BSP_LCD_WIDTH + x] = NAME_CARD_FG;    /* 下边 */
    }
    for (int y = 0; y < BSP_LCD_HEIGHT; y++) {
        fb[(size_t)y * BSP_LCD_WIDTH] = NAME_CARD_FG;                           /* 左边 */
        fb[(size_t)y * BSP_LCD_WIDTH + (BSP_LCD_WIDTH - 1)] = NAME_CARD_FG;     /* 右边 */
    }
#endif

    ESP_LOGI(TAG, "rendered \"%s\": %d glyphs %dx%d, area x=%d..%d y=%d..%d (frame=%d)",
             NAME_CARD_TEXT, NAME_GLYPH_COUNT, NAME_GLYPH_W, NAME_GLYPH_H,
             x0, x0 + total_w - 1, y0, y0 + NAME_GLYPH_H - 1, NAME_CARD_DIAG_FRAME);
}

esp_err_t name_card_show(void)
{
    uint16_t *fb = bsp_lcd_fb();

    if (fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    name_card_render(fb);
    return bsp_lcd_flush();     /* 整帧一次推送，内部等 DMA 完成 */
}
