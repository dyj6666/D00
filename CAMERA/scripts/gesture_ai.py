# -*- coding: utf-8 -*-
# gesture_ai.py —— AI 手势识别部署（tflite 模型）
# 前置：CAMERA/train/ 训练产物拷入 TF 卡根目录：
#   model_gesture.tflite + labels_gesture.txt
# 流程：肤色锁定手部 → 裁剪 1.5x 方形区域 → 缩放 32x32 → tf.classify
# 输出：
#   print 实时标签（IDE 终端验证）
#   UART 0x01 坐标帧 + 0x03 AI 手势帧（id + 置信度 0-100）
import sensor
import image
import time
import tf
from machine import UART

# ---------- 配置 ----------
THRESHOLDS = [(35, 88, 0, 30, 5, 35)]
MIN_SIZE, MAX_SIZE = 25, 260           # 放宽上限（手近景可大于 220）
RATIO_MIN, RATIO_MAX = 0.5, 1.6
TRACK_DIST = 120        # 跟踪匹配距离（px），超过则重新选最大块
EMA_ALPHA = 0.6
MODEL = "/sd/model_gesture.tflite"
LABELS = "/sd/labels_gesture.txt"
CONF_MIN = 0.65         # 置信度低于此值输出 "?"
VOTE_N = 5              # 分类结果多数投票帧数（抑制帧间抖动）

# ---------- UART ----------
uart = None
try:
    uart = UART(2, baudrate=115200)
except Exception as e:
    print("UART init skipped:", e)

def pack(type_, data):
    s = type_
    for b in data:
        s = (s + b) & 0xFF
    return b'\xAA\x55' + bytes([type_, len(data)]) + data + bytes([s])

def pack_hand(detected, x, y, w, h):
    return pack(0x01, bytes([1 if detected else 0,
                             x & 0xFF, (x >> 8) & 0xFF,
                             y & 0xFF, (y >> 8) & 0xFF,
                             w & 0xFF, (w >> 8) & 0xFF,
                             h & 0xFF, (h >> 8) & 0xFF]))

def pack_gesture_ai(gid, conf):
    return pack(0x03, bytes([gid, conf & 0xFF]))

# ---------- 模型加载 ----------
labels = [line.rstrip() for line in open(LABELS)]
print("labels:", labels)
net = tf.load(MODEL, load_to_fb=True)
print("MODEL OK:", net)

# ---------- 初始化 ----------
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

ex_prev, ey_prev = None, None
vote_buf = []
clock = time.clock()
print("READY: AI 手势识别")
while True:
    clock.tick()
    img = sensor.snapshot()

    blobs = img.find_blobs(THRESHOLDS, pixels_threshold=150,
                           area_threshold=150, merge=False)
    # 候选：不合并（merge=False 让手与身体肤色有间隙时保持独立块）；
    # 运动锁定优先（手是移动目标，脸/身体静止）：选与上帧目标最近的候选
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
        if ex_prev is not None:
            # 跟踪：选离上帧目标最近的候选（距离阈值内），否则选最大
            best = min(cands, key=lambda b: abs(b.cx() - ex_prev) + abs(b.cy() - ey_prev))
            d = abs(best.cx() - ex_prev) + abs(best.cy() - ey_prev)
            if d > 120:
                best = max(cands, key=lambda b: b.area())  # 目标丢失：重新选最大
        else:
            best = max(cands, key=lambda b: b.area())

    if best:
        cx, cy = best.cx(), best.cy()
        if ex_prev is not None:
            cx = int(ex_prev * (1 - EMA_ALPHA) + cx * EMA_ALPHA)
            cy = int(ey_prev * (1 - EMA_ALPHA) + cy * EMA_ALPHA)
        ex_prev, ey_prev = cx, cy

        # 端部定位裁剪：手是肤色块"最粗"的一端 → 质心偏向手；
        # 裁剪中心沿质心方向外推 0.6 倍偏移 + 0.9x 边长，让框聚焦手端
        bw, bh = best.w(), best.h()
        box_cx = best.x() + bw / 2.0
        box_cy = best.y() + bh / 2.0
        dx = best.cx() - box_cx
        dy = best.cy() - box_cy
        cx = int(best.cx() + dx * 0.6)
        cy = int(best.cy() + dy * 0.6)
        side = int(max(bw, bh) * 0.9)
        x0 = max(0, cx - side // 2)
        y0 = max(0, cy - side // 2)
        x1 = min(img.width(), x0 + side)
        y1 = min(img.height(), y0 + side)
        roi = img.copy(roi=(x0, y0, x1 - x0, y1 - y0))
        # 非等比拉伸到精确 32x32（与训练端 PIL resize 一致——等比缩放会
        # 得到非 32x32 尺寸，classify 内部再处理导致分布偏移、恒出某类）
        sc_x = 32.0 / roi.width()
        sc_y = 32.0 / roi.height()
        roi32 = roi.copy(scale_x=sc_x, scale_y=sc_y)

        # 分类 + 多数投票平滑（VOTE_N 帧取众数，抑制帧间抖动）
        gid = 0xFF      # 低置信度/无目标（协议 0x03 定义 0xFF）
        conf = 0.0
        for obj in tf.classify(net, roi32):
            out = sorted(zip(labels, obj.output()), key=lambda x: x[1], reverse=True)
            conf = out[0][1]
            if conf >= CONF_MIN:
                gid = labels.index(out[0][0])
        vote_buf.append(gid if gid != 0xFF else -1)
        if len(vote_buf) > VOTE_N:
            vote_buf.pop(0)
        # 众数
        if vote_buf:
            vgid = max(set(vote_buf), key=vote_buf.count)
            vconf = sum(1 for v in vote_buf if v == vgid) * 100.0 / len(vote_buf)
            if vgid >= 0:
                label = labels[vgid]
                gid = vgid
                conf = vconf / 100.0
            else:
                label = "?"
        else:
            label = "?"
        print("[AI] %s = %.0f%% fps=%.1f" % (label, conf * 100, clock.fps()))
        if uart:
            uart.write(pack_gesture_ai(gid, int(conf * 100)))
        img.draw_rectangle(best.rect(), color=(255, 0, 0))
        img.draw_cross(cx, cy, color=(0, 255, 0))
        if uart:
            uart.write(pack_hand(True, cx, cy, best.w(), best.h()))
    else:
        ex_prev, ey_prev = None, None
        vote_buf.clear()
        if uart:
            uart.write(pack_hand(False, 0, 0, 0, 0))
            uart.write(pack_gesture_ai(0xFF, 0))
