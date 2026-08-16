# -*- coding: utf-8 -*-
# face_track_v3.py —— 人脸跟踪 v3（分级锁定，抗"手贴脸"合并干扰）
# v3 改进：IoU 分级决策
#   IoU >= 0.45            → 正常跟踪（防膨胀 1.5x）
#   0.25 <= IoU < 0.45     → 疑似"手/手臂与脸合并"：锁定上帧框（不跟随）
#   IoU < 0.25             → 无关干扰：忽略（HOLD 保持后清空）
# 效果：脸不动时，手贴脸/靠近 → 框保持原地稳定，不跳不膨胀；
#        脸移动 → 高重叠跟踪照常；脸移出后 HOLD 6 帧自动重新捕获。
# 边界：手贴脸时"框大小"保持，但肤色连通无法做语义分离（物理极限，
#       彻底解决需 AI 人脸模型/固件升级）。
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

IOU_TRACK = 0.45      # 正常跟踪阈值
IOU_LOCK = 0.25       # 锁定阈值（低于则忽略）
MAX_GROW = 1.5
HOLD_FRAMES = 6

prev_rect = None
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
print("READY: 脸放中央，伸手测试——框应保持不动")
while True:
    clock.tick()
    img = sensor.snapshot()

    blobs = img.find_blobs(THRESHOLDS, pixels_threshold=PIX_TH,
                           area_threshold=AREA_TH, merge=True)

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

    if not cands:
        # 无候选：HOLD 保持
        if prev_rect is not None and hold < HOLD_FRAMES:
            hold += 1
            img.draw_rectangle(prev_rect, color=(255, 128, 0))
            print("HOLD x=%d y=%d w=%d h=%d" % prev_rect)
        else:
            prev_rect = None
            cx_prev, cy_prev = None, None
            if int(clock.fps()) % 30 == 0:
                print("no face ... fps=%.1f" % clock.fps())
        continue

    if prev_rect is None:
        # 无历史：取面积最大候选
        best = max(cands, key=lambda b: b.area())
        x, y, w, h = best.rect()
        rect = (x, y, w, h)
        mode = "NEW"
    else:
        best = max(cands, key=lambda b: rect_iou(prev_rect, b.rect()))
        iou = rect_iou(prev_rect, best.rect())
        if iou >= IOU_TRACK:
            # 正常跟踪 + 防膨胀
            x, y, w, h = best.rect()
            pw, ph = prev_rect[2], prev_rect[3]
            w = min(w, int(pw * MAX_GROW))
            h = min(h, int(ph * MAX_GROW))
            x = best.cx() - w // 2
            y = best.cy() - h // 2
            x = max(0, x); y = max(0, y)
            w = min(w, W - x); h = min(h, H - y)
            rect = (x, y, w, h)
            mode = "TRACK"
        elif iou >= IOU_LOCK:
            # 疑似合并干扰：锁定上帧框（不跟随，防手贴脸带跑/膨胀）
            rect = prev_rect
            mode = "LOCK"
        else:
            # 无关：HOLD
            if hold < HOLD_FRAMES:
                hold += 1
                rect = prev_rect
                mode = "HOLD"
            else:
                rect = None
                mode = "RESET"

    if rect is None:
        prev_rect = None
        cx_prev, cy_prev = None, None
        continue

    x, y, w, h = rect
    cx, cy = x + w // 2, y + h // 2
    if cx_prev is not None:
        cx = int(cx_prev * 0.4 + cx * 0.6)
        cy = int(cy_prev * 0.4 + cy * 0.6)
    cx_prev, cy_prev = cx, cy

    color = (255, 0, 0) if mode in ("TRACK", "NEW") else (255, 128, 0)
    img.draw_rectangle(rect, color=color)
    img.draw_cross(cx, cy, color=(0, 255, 0))
    dx, dy = cx - W // 2, cy - H // 2
    print("%s cx=%d cy=%d dx=%+d dy=%+d w=%d h=%d fps=%.1f" %
          (mode, cx, cy, dx, dy, w, h, clock.fps()))

    prev_rect = rect
    hold = 0
