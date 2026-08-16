# -*- coding: utf-8 -*-
# face_track_v2.py —— 人脸区域跟踪 v2（IoU 目标锁定 + 防膨胀）
# 解决"手/手臂贴近脸被一起框"：
#   1. IoU 跟踪：与上一帧脸框重叠度最高的肤色块视为"脸"（目标连续锁定）
#   2. 防膨胀：输出框尺寸 = min(当前块, 上帧×1.5, MAX_SIZE)——手贴脸导致
#      合并块变大时，框不会跟着膨胀
#   3. 丢失保持：连续 N 帧无匹配时保持上一帧位置（抗遮挡/闪烁）
#   4. 其余过滤沿用 v1（ROI/尺寸/宽高比/面积）
# 边界说明：肤色方案检测"肤色区域"非"人脸语义"——手紧贴脸且像素连通时
# 无法彻底分离（物理极限）；本版通过跟踪连续性使输出框保持稳定在脸上。
import sensor
import image
import time

sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

THRESHOLDS = [(35, 88, 0, 30, 5, 35)]
PIX_TH, AREA_TH = 150, 150
MIN_SIZE, MAX_SIZE = 40, 220
MIN_AREA, MAX_AREA = 2000, 30000
RATIO_MIN, RATIO_MAX = 0.6, 1.4
ROI_X0, ROI_X1, ROI_Y0, ROI_Y1 = 0.15, 0.85, 0.08, 0.72

W, H = sensor.width(), sensor.height()
rx0, rx1 = int(W * ROI_X0), int(W * ROI_X1)
ry0, ry1 = int(H * ROI_Y0), int(H * ROI_Y1)

IOU_MIN = 0.25        # 与上帧最小重叠率（低于视为新目标/干扰）
MAX_GROW = 1.5        # 框允许的最大膨胀倍数（防手贴脸撑大）
HOLD_FRAMES = 6       # 丢失后保持帧数

prev_rect = None      # (x, y, w, h)
hold = 0
cx_prev, cy_prev = None, None


def rect_iou(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    ix = max(0, min(ax + aw, bx + bw) - max(ax, bx))
    iy = max(0, min(ay + ah, by + bh) - max(ay, by))
    inter = ix * iy
    union = aw * ah + bw * bh - inter
    return inter / union if union > 0 else 0.0


clock = time.clock()
print("READY: 脸放画面中央，可伸手测试抗干扰")
while True:
    clock.tick()
    img = sensor.snapshot()

    blobs = img.find_blobs(THRESHOLDS, pixels_threshold=PIX_TH,
                           area_threshold=AREA_TH, merge=True)

    # 1. 收集合格候选
    cands = []
    for b in blobs:
        w, h = b.w(), b.h()
        if not (MIN_SIZE <= w <= MAX_SIZE and MIN_SIZE <= h <= MAX_SIZE):
            continue
        if not (MIN_AREA <= b.area() <= MAX_AREA):
            continue
        if not (RATIO_MIN < w / h < RATIO_MAX):
            continue
        if not (rx0 < b.cx() < rx1 and ry0 < b.cy() < ry1):
            continue
        cands.append(b)

    best = None
    if cands:
        if prev_rect is not None:
            # 2. IoU 跟踪：选与上帧重叠最大的候选
            best = max(cands, key=lambda b: rect_iou(prev_rect, b.rect()))
            if rect_iou(prev_rect, best.rect()) < IOU_MIN:
                best = None   # 全部不重叠 → 视为干扰/新目标
        else:
            # 无历史：选面积最大
            best = max(cands, key=lambda b: b.area())

    if best is not None:
        x, y, w, h = best.rect()
        # 3. 防膨胀：限制相对上帧的膨胀
        if prev_rect is not None:
            pw, ph = prev_rect[2], prev_rect[3]
            w = min(w, int(pw * MAX_GROW))
            h = min(h, int(ph * MAX_GROW))
            # 中心保持对齐
            x = best.cx() - w // 2
            y = best.cy() - h // 2
        x = max(0, x); y = max(0, y)
        w = min(w, W - x); h = min(h, H - y)
        rect = (x, y, w, h)

        cx, cy = x + w // 2, y + h // 2
        if cx_prev is not None:
            cx = int(cx_prev * 0.4 + cx * 0.6)
            cy = int(cy_prev * 0.4 + cy * 0.6)
        cx_prev, cy_prev = cx, cy

        img.draw_rectangle(rect, color=(255, 0, 0))
        img.draw_cross(cx, cy, color=(0, 255, 0))
        dx, dy = cx - W // 2, cy - H // 2
        print("FACE cx=%d cy=%d dx=%+d dy=%+d w=%d h=%d fps=%.1f" %
              (cx, cy, dx, dy, w, h, clock.fps()))

        prev_rect = rect
        hold = 0
    else:
        # 4. 丢失保持：短暂保持上一帧位置
        if prev_rect is not None and hold < HOLD_FRAMES:
            hold += 1
            x, y, w, h = prev_rect
            img.draw_rectangle(prev_rect, color=(255, 128, 0))  # 橙色=保持
            print("HOLD x=%d y=%d w=%d h=%d" % (x, y, w, h))
        else:
            prev_rect = None
            cx_prev, cy_prev = None, None
            if int(clock.fps()) % 30 == 0:
                print("no face ... fps=%.1f" % clock.fps())
