#!/usr/bin/env python
"""make_ldd_placeholder.py —— 生成"占位"线稿插画（黑底白线 + 百合 + 标题框）

作用：在拿到真正的原图之前，先验证「图片 -> C 数组 -> LVGL 全屏显示」整条通路。
把它换成你的真图只要一步：
    1) 把真图存成 ESP/projects/01_bringup/tools/ldd.png（覆盖本脚本产物）
    2) python tools/img2c.py --src tools/ldd.png --name img_ldd \
           --out-c components/ui/src/img_ldd.c \
           --out-h components/ui/include/ui/img_ldd.h
    3) idf.py build && flash
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 540, 496
BG = (6, 6, 7)
LINE = (232, 236, 240)
TEAL = (58, 190, 180)


def bezier(p0, p1, p2, n=40):
    pts = []
    for i in range(n + 1):
        t = i / n
        u = 1 - t
        x = u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0]
        y = u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]
        pts.append((x, y))
    return pts


def curve(draw, p0, p1, p2, width=2, color=LINE, n=44):
    draw.line(bezier(p0, p1, p2, n), fill=color, width=width, joint="curve")


def lily(draw, cx, cy, r, rot=0.0, petals=5, width=2, color=LINE):
    """简笔百合：花瓣 + 花蕊"""
    for i in range(petals):
        a = rot + i * (2 * math.pi / petals)
        ex, ey = cx + math.cos(a) * r * 1.45, cy + math.sin(a) * r * 1.45
        mx, my = cx + math.cos(a) * r * 0.55, cy + math.sin(a) * r * 0.55
        nx, ny = -math.sin(a) * r * 0.42, math.cos(a) * r * 0.42
        draw.line(bezier((cx, cy), (mx + nx, my + ny), (ex, ey), 16),
                  fill=color, width=width, joint="curve")
        draw.line(bezier((cx, cy), (mx - nx, my - ny), (ex, ey), 16),
                  fill=color, width=max(1, width - 1), joint="curve")
    for i in range(3):
        a = rot + i * 2.1
        draw.line([(cx, cy), (cx + math.cos(a) * r * 0.5, cy + math.sin(a) * r * 0.5)],
                  fill=color, width=1)
        draw.ellipse([cx + math.cos(a) * r * 0.5 - 2, cy + math.sin(a) * r * 0.5 - 2,
                      cx + math.cos(a) * r * 0.5 + 2, cy + math.sin(a) * r * 0.5 + 2],
                     outline=color, width=1)


def main():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    cx = W // 2
    face_top, face_bot = 150, 400

    # ---- 头发：外层大轮廓 ----
    for k, off in enumerate([-150, -110, 110, 150]):
        curve(d, (cx + off * 0.35, 120), (cx + off * 1.15, 40), (cx + off * 0.95, 260),
              width=2)
    # 顶部发丝
    for i in range(-4, 5):
        x0 = cx + i * 26
        curve(d, (x0, 96), (x0 + i * 6, 40), (x0 + i * 22, 130), width=2)

    # ---- 脸：只画左右两侧 + 下巴（上方被头发挡住）----
    d.arc([cx - 96, face_top + 20, cx + 96, face_bot], start=100, end=260, fill=LINE, width=2)
    d.arc([cx - 92, face_top, cx + 92, face_bot - 30], start=150, end=390, fill=LINE, width=2)

    # ---- 刘海压在脸上 ----
    for i, (sx, ex) in enumerate([(-70, -18), (-30, 12), (16, 62), (58, 96)]):
        curve(d, (cx + sx, face_top + 6), (cx + (sx + ex) // 2, face_top + 62),
              (cx + ex, face_top + 96), width=2)

    # ---- 眼睛：半睁（两条微下垂的横线 + 小瞳孔）----
    for sgn in (-1, 1):
        ex = cx + sgn * 42
        ey = face_top + 120
        curve(d, (ex - 22, ey), (ex, ey - 4), (ex + 22, ey + 1),
              width=2)
        d.ellipse([ex - 5, ey + 6, ex + 5, ey + 18], outline=LINE, width=2)
        # 上眼睑阴影线
        d.line([(ex - 24, ey - 7), (ex + 24, ey - 5)], fill=LINE, width=1)

    # ---- 鼻与嘴 ----
    d.line([(cx + 4, face_top + 152), (cx - 2, face_top + 166)], fill=LINE, width=1)
    d.line([(cx - 12, face_top + 196), (cx + 14, face_top + 196)], fill=LINE, width=2)

    # ---- 颈与肩 ----
    curve(d, (cx - 34, face_bot - 24), (cx - 44, face_bot + 40), (cx - 96, face_bot + 96), width=2)
    curve(d, (cx + 34, face_bot - 24), (cx + 44, face_bot + 40), (cx + 96, face_bot + 96), width=2)
    curve(d, (cx - 96, face_bot + 96), (cx, face_bot + 118), (cx + 96, face_bot + 96), width=2)

    # ---- 百合（左下 / 右下各一簇）----
    lily(d, 108, 392, 26, rot=0.4)
    lily(d, 74, 430, 22, rot=1.1)
    lily(d, 150, 424, 20, rot=-0.5)
    lily(d, 434, 390, 26, rot=0.9)
    lily(d, 470, 428, 22, rot=-0.3)
    lily(d, 392, 424, 20, rot=0.6)

    # ---- 青绿点缀（上方偏左）----
    for i in range(9):
        x = 60 + i * 7
        y = 196 + math.sin(i * 0.7) * 7
        r = max(1, 4 - abs(i - 4))
        d.line([(x, y), (x + 16, y - 5)], fill=TEAL, width=r)

    # ---- 标题框 ----
    box = [104, 424, 436, 470]
    d.rectangle(box, outline=LINE, width=2)
    font_path = None
    for cand in ("C:/Windows/Fonts/georgia.ttf", "C:/Windows/Fonts/times.ttf",
                 "C:/Windows/Fonts/constan.ttf"):
        if os.path.exists(cand):
            font_path = cand
            break
    text = "LDD MUSIC"
    if font_path:
        font = ImageFont.truetype(font_path, 34)
    else:
        font = ImageFont.load_default()
    tw = d.textlength(text, font=font)
    d.text(((W - tw) / 2, box[1] + 8), text, fill=(245, 246, 248), font=font)

    # 轻微发光：模糊后叠加，模拟画面里的荧光感
    glow = img.filter(ImageFilter.GaussianBlur(1.2))
    out = Image.blend(img, glow, 0.28)

    dst = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ldd.png")
    out.save(dst)
    print("占位线稿 -> %s (%dx%d)" % (dst, W, H))
    return 0


if __name__ == "__main__":
    sys.exit(main())
