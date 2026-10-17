#!/usr/bin/env python
"""img2c.py —— 把 PNG/JPG 转成 LVGL 可直接显示的 RGB565 C 数组

为什么用位图：线稿/插画这类高密度图形用 LVGL 图元重画不现实（几百条细曲线），
转成位图后是「一次 blit」，成本与填充屏幕相当，画质还能保留原图（含渐变、光晕）。

用法：
    python tools/img2c.py --src tools/ldd.png --name img_ldd \
        --out-c components/ui/src/img_ldd.c \
        --out-h components/ui/include/ui/img_ldd.h \
        --w 320 --h 240

参数：
    --w/--h   目标尺寸（默认 320x240；用 Lanczos 高质量缩小，细线才不会断）
    --fit     contain（等比缩放+黑边）| cover（等比缩放+裁切，默认）| stretch
    --bg      可选：把该颜色（如 000000）当背景做的透明键（仅提示，不抠图）
输出：
    C 数组（RGB565 小端）+ lv_img_dsc_t 描述符 + 头文件声明
"""
import argparse
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    print("需要 Pillow：D:\\Python\\python.exe -m pip install pillow", file=sys.stderr)
    sys.exit(2)


def load_fitted(src, w, h, fit):
    im = Image.open(src).convert("RGB")
    sw, sh = im.size
    if fit == "stretch":
        return im.resize((w, h), Image.LANCZOS)
    sx, sy = w / sw, h / sh
    scale = max(sx, sy) if fit == "cover" else min(sx, sy)
    nw, nh = max(1, round(sw * scale)), max(1, round(sh * scale))
    im = im.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGB", (w, h), (0, 0, 0))
    canvas.paste(im, ((w - nw) // 2, (h - nh) // 2))
    return canvas


def to_rgb565(im):
    px = im.load()
    w, h = im.size
    out = bytearray(w * h * 2)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            struct.pack_into("<H", out, i, v)
            i += 2
    return bytes(out)


def emit_c(name, data, w, h, out_c, out_h):
    guard = os.path.basename(out_h).upper().replace(".", "_").replace("-", "_")
    os.makedirs(os.path.dirname(out_c) or ".", exist_ok=True)
    os.makedirs(os.path.dirname(out_h) or ".", exist_ok=True)

    with open(out_c, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* 由 tools/img2c.py 生成，勿手改 */\n")
        f.write('#include "lvgl.h"\n')
        f.write('#include "%s"\n\n' % os.path.basename(out_h))
        f.write("static const uint8_t %s_map[] = {\n" % name)
        for off in range(0, len(data), 16):
            chunk = data[off:off + 16]
            f.write("    " + ",".join("0x%02X" % b for b in chunk) + ",\n")
        f.write("};\n\n")
        f.write("const lv_img_dsc_t %s = {\n" % name)
        f.write("    .header.always_zero = 0,\n")
        f.write("    .header.w = %d,\n" % w)
        f.write("    .header.h = %d,\n" % h)
        f.write("    .header.cf = LV_IMG_CF_TRUE_COLOR,\n")
        f.write("    .data_size = %d,\n" % len(data))
        f.write("    .data = %s_map,\n" % name)
        f.write("};\n")

    with open(out_h, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* 由 tools/img2c.py 生成，勿手改 */\n")
        f.write("#ifndef %s\n#define %s\n\n" % (guard, guard))
        f.write('#include "lvgl.h"\n\n')
        f.write("extern const lv_img_dsc_t %s;\n\n" % name)
        f.write("#endif /* %s */\n" % guard)

    return len(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--out-c", required=True)
    ap.add_argument("--out-h", required=True)
    ap.add_argument("--w", type=int, default=320)
    ap.add_argument("--h", type=int, default=240)
    ap.add_argument("--fit", default="cover", choices=["cover", "contain", "stretch"])
    ap.add_argument("--bg", default="")
    args = ap.parse_args()

    im = load_fitted(args.src, args.w, args.h, args.fit)
    data = to_rgb565(im)
    n = emit_c(args.name, data, args.w, args.h, args.out_c, args.out_h)

    kb = n / 1024.0
    print("源图 %s" % args.src)
    print("输出 %dx%d RGB565 = %.1f KB flash" % (args.w, args.h, kb))
    print("  -> %s" % args.out_c)
    print("  -> %s" % args.out_h)
    if kb > 400:
        print("警告：体积偏大，考虑降尺寸或换 ALPHA_4BIT", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
