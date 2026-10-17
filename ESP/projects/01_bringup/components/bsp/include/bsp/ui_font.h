/* ================================================================
 * ui_font.h —— 板载 UI 点阵字库（自动生成，请勿手改）
 *
 * 生成工具：tools/gen_ui_font.py
 * 字体来源：simhei.ttf
 * 单元尺寸：ASCII 8x16，汉字 16x16，1bpp 行优先、MSB 在左
 * 覆盖范围：ASCII 0x20~0x7E 共 95 个；汉字 88 个（UI 用字全量）
 * ================================================================ */
#ifndef __BSP_UI_FONT_H
#define __BSP_UI_FONT_H

#include <stddef.h>     /* NULL */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_UI_FONT_HEIGHT      16      /*!< 行高（两种单元同高） */
#define BSP_UI_ASCII_WIDTH      8      /*!< 半角宽度 */
#define BSP_UI_CJK_WIDTH        16      /*!< 全角宽度 */

/** 单个字形 */
typedef struct
{
    uint32_t       codepoint;   /*!< Unicode 码点 */
    uint8_t        width;       /*!< 像素宽（8 或 16） */
    uint8_t        height;      /*!< 像素高（16） */
    uint8_t        stride;      /*!< 每行字节数 */
    const uint8_t *bits;        /*!< 点阵，行优先，MSB 在左 */
} bsp_ui_glyph_t;

extern const bsp_ui_glyph_t bsp_ui_ascii[95];
extern const bsp_ui_glyph_t bsp_ui_cjk[88];
extern const uint16_t       bsp_ui_cjk_count;

/**
 * @brief  按码点查字形
 * @return 字形指针；未收录返回 NULL（绘制方会画成实心方块提示缺字）
 */
const bsp_ui_glyph_t *bsp_ui_font_lookup(uint32_t codepoint);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_UI_FONT_H */
