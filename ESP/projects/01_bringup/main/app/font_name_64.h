/* ================================================================
 * font_name_64.h —— 名字卡汉字字模（自动生成，请勿手改）
 *
 * 由 tools/gen_font.py 生成；改字/改字号请改脚本后重新运行，
 * 不要在 C 源码里手改位图数据。
 * ================================================================ */
#ifndef __FONT_NAME_64_H
#define __FONT_NAME_64_H

#include <stdint.h>

#define NAME_GLYPH_W        64      /* 单字宽（像素） */
#define NAME_GLYPH_H        64      /* 单字高（像素） */
#define NAME_GLYPH_COUNT    3       /* 字模个数 */

/* 单个字形：Unicode 码点 + 指向 8bit 覆盖率位图的指针 */
typedef struct
{
    uint32_t        codepoint;
    const uint8_t  *alpha;
} name_glyph_t;

extern const name_glyph_t name_glyphs[NAME_GLYPH_COUNT];

#endif
