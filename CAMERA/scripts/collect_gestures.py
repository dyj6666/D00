# -*- coding: utf-8 -*-
# collect_gestures.py —— 手势数据采集（OpenART 侧）
# 用法：
#   1. 改 CLASS 为当前要采集的手势名（如 "fist"、"palm"、"two"）
#   2. 运行 → 把手比好手势放到镜头前（保持不动 15 秒左右）
#   3. 脚本自动裁剪手部区域并保存到 TF 卡 /sd/gesture/<CLASS>/xxx.jpg
#   4. 换下一个手势（改 CLASS 重新运行），每类采集 50~80 张
# 采集完：TF 卡 gesture/ 目录整体拷到电脑 CAMERA/train/dataset/ 交给训练
import sensor
import image
import time
import os
import random

# ---------- 采集配置 ----------
CLASS = "victory"          # ← 每次运行前改成当前手势名（英文小写）
COUNT = 60              # 采集张数（每 0.25s 一张，约 15 秒）
SAVE_EVERY_MS = 250

THRESHOLDS = [(35, 88, 0, 30, 5, 35)]
MIN_SIZE, MAX_SIZE = 30, 220
RATIO_MIN, RATIO_MAX = 0.5, 1.6

# ---------- 初始化 ----------
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

dir_path = "/sd/gesture/" + CLASS
try:
    os.mkdir("/sd/gesture")
except OSError:
    pass
try:
    os.mkdir(dir_path)
except OSError:
    pass

saved = 0
last_save = 0
clock = time.clock()
print("COLLECT [%s] 目标 %d 张，请把手比好手势放镜头前（可缓慢微动）" % (CLASS, COUNT))
while saved < COUNT:
    clock.tick()
    now = time.ticks_ms()
    img = sensor.snapshot()

    blobs = img.find_blobs(THRESHOLDS, pixels_threshold=150,
                           area_threshold=150, merge=True)
    best, best_area = None, 0
    for b in blobs:
        w, h = b.w(), b.h()
        if not (MIN_SIZE <= w <= MAX_SIZE and MIN_SIZE <= h <= MAX_SIZE):
            continue
        if not (RATIO_MIN < w / h < RATIO_MAX):
            continue
        if b.area() > best_area:
            best, best_area = b, b.area()

    if best and now - last_save >= SAVE_EVERY_MS:
        # 以手中心裁剪正方形区域（1.5 倍边长，留背景余量）
        cx, cy = best.cx(), best.cy()
        side = int(max(best.w(), best.h()) * 1.5)
        # 多样化：裁剪框随机偏移/缩放（模拟手在画面不同位置/大小，
        # 提升模型对位置变化的鲁棒性）
        jx = random.randint(-int(side * 0.35), int(side * 0.35))
        jy = random.randint(-int(side * 0.35), int(side * 0.35))
        side = int(side * random.uniform(0.75, 1.25))
        x0 = max(0, cx + jx - side // 2)
        y0 = max(0, cy + jy - side // 2)
        x1 = min(img.width(), x0 + side)
        y1 = min(img.height(), y0 + side)
        roi = img.copy(roi=(x0, y0, x1 - x0, y1 - y0))
        # V4.3 无 resize：直接存裁剪块（尺寸不一，训练端统一缩 32x32）
        path = "%s/%03d.jpg" % (dir_path, saved)
        roi.save(path, quality=88)
        saved += 1
        last_save = now
        img.draw_rectangle(best.rect(), color=(255, 0, 0))
        img.draw_rectangle((x0, y0, x1 - x0, y1 - y0), color=(0, 255, 0))
        print("[%d/%d] saved %s" % (saved, COUNT, path))

print("COLLECT DONE [%s] %d 张，换下一个手势改 CLASS 重跑" % (CLASS, saved))
