#!/usr/bin/env python3
# ================================================================
# gen_ui_font.py —— 生成板载 UI 点阵字库（ASCII 8x16 + 中文 16x16）
#
# 为什么要自带字库：板子上没有字库芯片，SD 卡也不一定插着；
# 而开机自检动画必须"不依赖任何外部资源"就能显示文字。
# 这里把用到的字形离线渲染成 1bpp 点阵编进固件（约 3KB），最省事也最稳。
#
# 中文取字规则（保证不遗漏）：
#   1. 显式列表 CJK_EXTRA（兜底，一定包含）；
#   2. 自动扫描 UI 源码里中文字符串字面量中的汉字（先剥注释，避免把注释里的字也算进来）。
#   两路取并集 —— 屏幕上出现过的字，字库里一定有。
#
# 用法：D:\Python\python.exe tools\gen_ui_font.py
# ================================================================
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

# ---------------- 可调参数 ----------------
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\simhei.ttf",      # 黑体：小字号下笔画清晰，点阵化效果好
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\simsun.ttc",
]

ASCII_W, ASCII_H = 8, 16       # 半角单元
CJK_W, CJK_H = 16, 16          # 全角单元
THRESHOLD = 110                # 灰度阈值（偏低一点，避免小字号笔画断掉）

# 显式兜底列表（UI 里用到的全部汉字）
CJK_EXTRA = (
    "开机自检屏幕触摸按键灯光蜂鸣扩展存储光感六轴温度时钟采样串口卡音频"
    "通过失败待确认跳完成重新间无秒"
)

# 需要扫描的源码（自动补字）
SCAN_SOURCES = [
    "main/app/boot_ui.c",
    "components/bsp/src/lcd.c",
]

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_H = os.path.join(PROJ, "components", "bsp", "include", "bsp", "ui_font.h")
OUT_C = os.path.join(PROJ, "components", "bsp", "src", "ui_font.c")


def pick_font():
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return path
    raise SystemExit("未找到可用中文字体，请修改 FONT_CANDIDATES")


def strip_comments(text):
    """剥掉 C 注释，避免把注释里的中文当成 UI 用字。"""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def collect_cjk_from_sources():
    """扫描源码字符串字面量里的汉字。"""
    found = set()
    for rel in SCAN_SOURCES:
        path = os.path.join(PROJ, rel)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as fp:
            code = strip_comments(fp.read())
        for literal in re.findall(r'"((?:[^"\\]|\\.)*)"', code):
            for ch in literal:
                if ord(ch) > 0x7F:
                    found.add(ch)
    return found


def render_ascii(ch, font_path):
    """渲染一个 ASCII 字符到 8x16 单元（左基线对齐，保证基线一致）。"""
    if ch == " ":
        return Image.new("L", (ASCII_W, ASCII_H), 0)

    big = Image.new("L", (ASCII_W * 3, ASCII_H * 3), 0)
    font = ImageFont.truetype(font_path, ASCII_H)
    draw = ImageDraw.Draw(big)
    draw.text((ASCII_W, ASCII_H * 2), ch, font=font, fill=255, anchor="ls")
    # 以"基线在单元内 y = ASCII_H - 3"的方式裁出 8x16
    box = (ASCII_W - 0, ASCII_H * 2 - (ASCII_H - 3),
           ASCII_W - 0 + ASCII_W, ASCII_H * 2 - (ASCII_H - 3) + ASCII_H)
    return big.crop(box)


def render_cjk(ch, font_path):
    """渲染一个汉字到 16x16 单元（居中，不裁剪以保持字面一致）。"""
    img = Image.new("L", (CJK_W, CJK_H), 0)
    font = ImageFont.truetype(font_path, CJK_H)
    draw = ImageDraw.Draw(img)
    draw.text((CJK_W // 2, CJK_H // 2 + 1), ch, font=font, fill=255, anchor="mm")
    return img


def pack_1bpp(img):
    """把灰度图阈值化成 1bpp 行优先点阵（MSB 在左）。"""
    w, h = img.size
    stride = (w + 7) // 8
    px = img.load()
    out = bytearray()
    for y in range(h):
        for bx in range(stride):
            byte = 0
            for bit in range(8):
                x = bx * 8 + bit
                if x < w and px[x, y] >= THRESHOLD:
                    byte |= (0x80 >> bit)
            out.append(byte)
    return bytes(out), stride


def emit(glyphs, font_path):
    """输出 .h 与 .c。"""
    ascii_glyphs = glyphs["ascii"]
    cjk_glyphs = glyphs["cjk"]

    header = """/* ================================================================
 * ui_font.h —— 板载 UI 点阵字库（自动生成，请勿手改）
 *
 * 生成工具：tools/gen_ui_font.py
 * 字体来源：%s
 * 单元尺寸：ASCII %dx%d，汉字 %dx%d，1bpp 行优先、MSB 在左
 * 覆盖范围：ASCII 0x20~0x7E 共 %d 个；汉字 %d 个（UI 用字全量）
 * ================================================================ */
#ifndef __BSP_UI_FONT_H
#define __BSP_UI_FONT_H

#include <stddef.h>     /* NULL */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_UI_FONT_HEIGHT      %d      /*!< 行高（两种单元同高） */
#define BSP_UI_ASCII_WIDTH      %d      /*!< 半角宽度 */
#define BSP_UI_CJK_WIDTH        %d      /*!< 全角宽度 */

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
extern const bsp_ui_glyph_t bsp_ui_cjk[%d];
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
""" % (os.path.basename(font_path), ASCII_W, ASCII_H, CJK_W, CJK_H,
       len(ascii_glyphs), len(cjk_glyphs),
       ASCII_H, ASCII_W, CJK_W, len(cjk_glyphs))

    lines = []
    lines.append("/* ================================================================")
    lines.append(" * ui_font.c —— 板载 UI 点阵字库（自动生成，请勿手改）")
    lines.append(" *")
    lines.append(" * 由 tools/gen_ui_font.py 生成；字体：%s" % os.path.basename(font_path))
    lines.append(" * 1bpp 行优先、MSB 在左；0 = 不亮，1 = 亮")
    lines.append(" * ================================================================ */")
    lines.append("")
    lines.append('#include "bsp/ui_font.h"')
    lines.append("")
    lines.append("#include <stddef.h>   /* NULL（bsp_ui_font_lookup 未命中时返回） */")
    lines.append("")

    def emit_array(name, items, w):
        for cp, data in items:
            lines.append("/* U+%04X */" % cp)
            lines.append("static const uint8_t %s_%04X[%d] = {" % (name, cp, len(data)))
            for i in range(0, len(data), 16):
                lines.append("    " + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",")
            lines.append("};")
            lines.append("")

    emit_array("s_a", ascii_glyphs, ASCII_W)
    emit_array("s_c", cjk_glyphs, CJK_W)

    lines.append("/* ASCII 0x20 ~ 0x7E */")
    lines.append("const bsp_ui_glyph_t bsp_ui_ascii[95] = {")
    for cp, _ in ascii_glyphs:
        lines.append("    { 0x%04X, %d, %d, %d, s_a_%04X }," % (cp, ASCII_W, ASCII_H, (ASCII_W + 7) // 8, cp))
    lines.append("};")
    lines.append("")

    lines.append("/* 汉字表（UI 用字全量） */")
    lines.append("const bsp_ui_glyph_t bsp_ui_cjk[%d] = {" % len(cjk_glyphs))
    for cp, _ in cjk_glyphs:
        lines.append("    { 0x%04X, %d, %d, %d, s_c_%04X }," % (cp, CJK_W, CJK_H, (CJK_W + 7) // 8, cp))
    lines.append("};")
    lines.append("")
    lines.append("const uint16_t bsp_ui_cjk_count = %d;" % len(cjk_glyphs))
    lines.append("")
    lines.append("const bsp_ui_glyph_t *bsp_ui_font_lookup(uint32_t codepoint)")
    lines.append("{")
    lines.append("    if (codepoint >= 0x20U && codepoint <= 0x7EU) {")
    lines.append("        return &bsp_ui_ascii[codepoint - 0x20U];")
    lines.append("    }")
    lines.append("    for (uint16_t i = 0; i < bsp_ui_cjk_count; i++) {")
    lines.append("        if (bsp_ui_cjk[i].codepoint == codepoint) {")
    lines.append("            return &bsp_ui_cjk[i];")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return NULL;")
    lines.append("}")

    with open(OUT_H, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(header)
    with open(OUT_C, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(lines) + "\n")


def main():
    font_path = pick_font()

    # ---- 确定汉字集合 ----
    chars = set(CJK_EXTRA)
    scanned = collect_cjk_from_sources()
    chars |= scanned
    chars = sorted(chars)

    # ---- 渲染 ----
    ascii_glyphs = []
    for cp in range(0x20, 0x7F):
        img = render_ascii(chr(cp), font_path)
        data, _ = pack_1bpp(img)
        ascii_glyphs.append((cp, data))

    cjk_glyphs = []
    blank = []
    for ch in chars:
        img = render_cjk(ch, font_path)
        data, _ = pack_1bpp(img)
        if not any(data):
            blank.append(ch)
        cjk_glyphs.append((ord(ch), data))
    if blank:
        raise SystemExit("以下字渲染为空白，请检查字体：%s" % "".join(blank))

    emit({"ascii": ascii_glyphs, "cjk": cjk_glyphs}, font_path)

    print("字体  : %s" % font_path)
    print("ASCII : %d 个（0x20~0x7E, %dx%d）" % (len(ascii_glyphs), ASCII_W, ASCII_H))
    print("汉字  : %d 个（%dx%d）-> %s" % (len(cjk_glyphs), CJK_W, CJK_H, "".join(chars)))
    print("扫描到: %d 个（来自 %s）" % (len(scanned), ", ".join(SCAN_SOURCES)))
    print("输出  : %s / %s" % (OUT_C, OUT_H))


if __name__ == "__main__":
    sys.exit(main())
