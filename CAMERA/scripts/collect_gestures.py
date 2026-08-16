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
prev_cx, prev_cy, prev_size = None, None, 0
clock = time.clock()
print("COLLECT [%s] 目标 %d 张，请把手比好手势放镜头前（可缓慢微动）" % (CLASS, COUNT))
while saved < COUNT:
    clock.tick()
    now = time.ticks_ms()
    img = sensor.snapshot()

    blobs = img.find_blobs(THRESHOLDS, pixels_threshold=150,
                           area_threshold=150, merge=False)
    # 手位置记忆跟踪：手独立时记住位置/大小；手移到中间与身体连通
    # （大块）时，用记忆位置继续框住手——解决"换位置就不采集"
    cands = []
    for b in blobs:
        w, h = b.w(), b.h()
        if not (MIN_SIZE <= w <= MAX_SIZE and MIN_SIZE <= h <= MAX_SIZE):
            continue
        if not (RATIO_MIN < w / h < RATIO_MAX):
            continue
        cands.append(b)

    best = None
    if cands:
        if prev_cx is not None:
            best = min(cands, key=lambda b: abs(b.cx() - prev_cx) + abs(b.cy() - prev_cy))
            d = abs(best.cx() - prev_cx) + abs(best.cy() - prev_cy)
            if d > 300:
                best = max(cands, key=lambda b: b.area())  # 记忆丢失：选最大
        else:
            best = max(cands, key=lambda b: b.area())

    use_mem = False
    if best is not None:
        is_hand = best.area() < 20000 and max(best.w(), best.h()) < 160
        if is_hand:
            # 手级独立块：更新位置/大小记忆
            prev_cx, prev_cy = best.cx(), best.cy()
            prev_size = max(best.w(), best.h())
        elif prev_cx is not None and prev_size > 0:
            # 身体级大块（手与身体肤色连通）：用手记忆位置继续框/采
            use_mem = True
        else:
            # 无记忆且只有大块：退化用块中心（并建立记忆）
            prev_cx, prev_cy = best.cx(), best.cy()
            prev_size = max(best.w(), best.h())
    else:
        # 无候选：短暂保留记忆（500ms 后清空）
        if prev_cx is not None and (now - last_save) > 500:
            prev_cx = prev_cy = None
            prev_size = 0

    # 确定裁剪中心与边长
    if use_mem:
        cx, cy = prev_cx, prev_cy
        side = int(prev_size * 1.1)
    elif best is not None:
        # 手级块：端部定位（质心偏手端 + 外推 0.6 + 0.9x）
        bw, bh = best.w(), best.h()
        box_cx = best.x() + bw / 2.0
        box_cy = best.y() + bh / 2.0
        dx = best.cx() - box_cx
        dy = best.cy() - box_cy
        cx = int(best.cx() + dx * 0.6)
        cy = int(best.cy() + dy * 0.6)
        side = int(max(bw, bh) * 0.9)
    else:
        cx = cy = side = 0

    if side > 0 and now - last_save >= SAVE_EVERY_MS:
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
