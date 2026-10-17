#!/usr/bin/env python3
# ================================================================
# gen_font.py —— 生成点名字卡用的汉字灰度字模（C 源码）
#
# 为什么不用 LVGL / 外置字库：
#   本工程是 bringup 阶段，只需在屏幕上显示 3 个汉字（董/衍/俊）。
#   引入 LVGL + 全量 GBK 字库（或 SD 卡字库）成本和风险都远大于收益，
#   因此这里把需要的字形离线渲染成 8bit 灰度位图，直接编进固件。
#
# 输入：系统 CJK 字体（默认黑体 simhei.ttf）+ 字符列表
# 输出：main/app/font_name_64.c / .h（8bit 覆盖率，供上屏时做抗锯齿混合）
#       以及 build/font_preview/*.png（仅供人眼核对，不入库）
#
# 用法（在工程根目录下执行）：D:\Python\python.exe tools\gen_font.py
# ================================================================
import os
import sys

from PIL import Image, ImageDraw, ImageFont

# ---------------- 可调参数 ----------------
GLYPH_PX = 64                      # 单个字模边长（像素）
CHARS = "董衍俊"                     # 要显示的名字
FONT_CANDIDATES = [                # 依次尝试，取第一个存在的
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\msyhbd.ttc",
    r"C:\Windows\Fonts\msyh.ttc",
]

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_C = os.path.join(PROJ, "main", "app", "font_name_64.c")
OUT_H = os.path.join(PROJ, "main", "app", "font_name_64.h")
PREVIEW_DIR = os.path.join(PROJ, "build", "font_preview")

SCREEN_W, SCREEN_H = 320, 240      # 面板横屏分辨率
SPACING_PX = 10                    # 字间距
BG_RGB = (255, 255, 255)           # 背景：白
FG_RGB = (0, 64, 160)              # 前景：深蓝


def pick_font_path():
    """返回第一个可用的中文字体路径。"""
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return path
    raise SystemExit("未找到可用的中文字体，请修改 FONT_CANDIDATES")


def render_glyph(ch, font_path):
    """把单个汉字渲染成 GLYPH_PX x GLYPH_PX 的 8bit 灰度位图。

    先在大画布上居中绘制，再按墨迹外接框裁剪并居中贴回目标尺寸，
    这样不同汉字的字形大小/边距能保持一致，不会因字体自身留白而跑偏。
    """
    big = GLYPH_PX * 3
    canvas = Image.new("L", (big, big), 0)
    font = ImageFont.truetype(font_path, GLYPH_PX)
    draw = ImageDraw.Draw(canvas)
    draw.text((big // 2, big // 2), ch, font=font, fill=255, anchor="mm")

    ink = canvas.getbbox()                    # 墨迹外接框（无墨迹时为 None）
    if ink is None:
        raise SystemExit("字形渲染为空白：%s" % ch)

    glyph = canvas.crop(ink)
    # 墨迹超过目标尺寸时等比缩小，保证不裁掉笔画
    limit = GLYPH_PX
    if glyph.width > limit or glyph.height > limit:
        scale = min(limit / glyph.width, limit / glyph.height)
        glyph = glyph.resize(
            (max(1, int(glyph.width * scale)), max(1, int(glyph.height * scale))),
            Image.LANCZOS,
        )

    out = Image.new("L", (GLYPH_PX, GLYPH_PX), 0)
    out.paste(glyph, ((GLYPH_PX - glyph.width) // 2, (GLYPH_PX - glyph.height) // 2))
    return out


def emit_c_source(glyphs, font_path):
    """输出字形数组 + 索引表（C 源文件）。"""
    lines = [
        "/* ================================================================",
        " * font_name_64.c —— 名字卡汉字字模（自动生成，请勿手改）",
        " *",
        " * 生成工具：tools/gen_font.py（Pillow 渲染，抗锯齿 8bit 覆盖率）",
        " * 字体来源：%s" % os.path.basename(font_path),
        " * 单字尺寸：%d x %d 像素，0 = 全透明，255 = 全不透明" % (GLYPH_PX, GLYPH_PX),
        " * 覆盖字符：%s" % CHARS,
        " * ================================================================ */",
        "",
        '#include "font_name_64.h"',
        "",
    ]

    for ch, img in glyphs:
        data = list(img.tobytes())
        lines.append("/* '%s' U+%04X 的覆盖率位图（逐行，每行 %d 字节） */" % (ch, ord(ch), GLYPH_PX))
        lines.append("static const uint8_t s_alpha_%04X[%d] = {" % (ord(ch), len(data)))
        for i in range(0, len(data), 16):
            row = ", ".join("0x%02X" % v for v in data[i:i + 16])
            lines.append("    %s," % row)
        lines.append("};")
        lines.append("")

    lines.append("/* 字符 -> 位图 映射表（顺序即显示顺序） */")
    lines.append("const name_glyph_t name_glyphs[NAME_GLYPH_COUNT] = {")
    for ch, _ in glyphs:
        lines.append("    { 0x%04X, s_alpha_%04X }," % (ord(ch), ord(ch)))
    lines.append("};")
    lines.append("")

    with open(OUT_C, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(lines))


def emit_c_header():
    """输出字模对外接口。"""
    text = """/* ================================================================
 * font_name_64.h —— 名字卡汉字字模（自动生成，请勿手改）
 *
 * 由 tools/gen_font.py 生成；改字/改字号请改脚本后重新运行，
 * 不要在 C 源码里手改位图数据。
 * ================================================================ */
#ifndef __FONT_NAME_64_H
#define __FONT_NAME_64_H

#include <stdint.h>

#define NAME_GLYPH_W        %d      /* 单字宽（像素） */
#define NAME_GLYPH_H        %d      /* 单字高（像素） */
#define NAME_GLYPH_COUNT    3       /* 字模个数 */

/* 单个字形：Unicode 码点 + 指向 8bit 覆盖率位图的指针 */
typedef struct
{
    uint32_t        codepoint;
    const uint8_t  *alpha;
} name_glyph_t;

extern const name_glyph_t name_glyphs[NAME_GLYPH_COUNT];

#endif
""" % (GLYPH_PX, GLYPH_PX)

    with open(OUT_H, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(text)


def emit_preview(glyphs):
    """按面板实际坐标生成一张预览图，供人工核对排版（不入库）。"""
    os.makedirs(PREVIEW_DIR, exist_ok=True)

    total_w = len(glyphs) * GLYPH_PX + (len(glyphs) - 1) * SPACING_PX
    x0 = (SCREEN_W - total_w) // 2
    y0 = (SCREEN_H - GLYPH_PX) // 2

    screen = Image.new("RGB", (SCREEN_W, SCREEN_H), BG_RGB)
    for idx, (_, img) in enumerate(glyphs):
        piece = Image.new("RGB", (GLYPH_PX, GLYPH_PX), BG_RGB)
        piece.paste(Image.new("RGB", (GLYPH_PX, GLYPH_PX), FG_RGB), (0, 0), img)
        screen.paste(piece, (x0 + idx * (GLYPH_PX + SPACING_PX), y0))

    screen_path = os.path.join(PREVIEW_DIR, "preview_screen.png")
    screen.resize((SCREEN_W * 2, SCREEN_H * 2), Image.NEAREST).save(screen_path)
    print("预览图：%s" % screen_path)
    return x0, y0, total_w


def main():
    font_path = pick_font_path()
    glyphs = [(ch, render_glyph(ch, font_path)) for ch in CHARS]
    emit_c_header()
    emit_c_source(glyphs, font_path)

    x0, y0, total_w = emit_preview(glyphs)
    print("字体：%s" % font_path)
    print("字模：%d x %d x %d 字 -> %s" % (GLYPH_PX, GLYPH_PX, len(glyphs), OUT_C))
    print("排版：x=%d..%d, y=%d..%d（屏 %dx%d）"
          % (x0, x0 + total_w - 1, y0, y0 + GLYPH_PX - 1, SCREEN_W, SCREEN_H))


if __name__ == "__main__":
    sys.exit(main())
