#!/usr/bin/env python3
# ================================================================
# check_font.py —— 校验生成出来的字模数据（gen_font.py 的自检工具）
#
# 用途：不依赖上网/不依赖屏幕，直接把 main/app/font_name_64.c 里的
#       数组解析回位图，报告每个字的墨迹量并打印 ASCII 点阵。
#       用于区分"字模数据本身是空的"和"上屏渲染有问题"。
#
# 用法：D:\Python\python.exe tools\check_font.py
# ================================================================
import os
import re

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(PROJ, "main", "app", "font_name_64.c")

ART_CHARS = " .+@"          # 由浅到深的四级灰度


def main():
    with open(SRC, encoding="utf-8") as fp:
        text = fp.read()

    blocks = re.findall(
        r"static const uint8_t (s_alpha_[0-9A-F]+)\[(\d+)\]\s*=\s*\{(.*?)\};",
        text, re.S)
    if not blocks:
        raise SystemExit("没有在 %s 里找到字模数组" % SRC)

    print("文件：%s（%d 字节）" % (SRC, len(text)))
    for name, declared, body in blocks:
        vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
        ink = sum(1 for v in vals if v >= 128)
        rows = [y for y in range(64) if any(vals[y * 64:(y + 1) * 64])]
        print("%-14s 声明=%-5s 解析=%-5d 非零=%-5d 墨迹=%-5d 行范围=%s"
              % (name, declared, len(vals), sum(1 for v in vals if v), ink,
                 ("%d..%d" % (rows[0], rows[-1])) if rows else "空"))

    print("\n--- ASCII 点阵（每两行合一行显示）---")
    for name, _, body in blocks:
        vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
        print("==== %s ====" % name)
        for y in range(0, 64, 2):
            row = ""
            for x in range(64):
                v = max(vals[y * 64 + x], vals[(y + 1) * 64 + x])
                row += ART_CHARS[0 if v < 16 else 1 if v < 60 else 2 if v < 140 else 3]
            print(row)


if __name__ == "__main__":
    main()
