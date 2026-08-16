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
MIN_SIZE, MAX_SIZE = 25, 220
RATIO_MIN, RATIO_MAX = 0.5, 1.6
EMA_ALPHA = 0.6
MODEL = "/sd/model_gesture.tflite"
LABELS = "/sd/labels_gesture.txt"
CONF_MIN = 0.6          # 置信度低于此值输出 "?"

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
clock = time.clock()
print("READY: AI 手势识别")
while True:
    clock.tick()
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

    if best:
        cx, cy = best.cx(), best.cy()
        if ex_prev is not None:
            cx = int(ex_prev * (1 - EMA_ALPHA) + cx * EMA_ALPHA)
            cy = int(ey_prev * (1 - EMA_ALPHA) + cy * EMA_ALPHA)
        ex_prev, ey_prev = cx, cy

        # 裁剪手部方形区域（1.5x），缩放 32x32（V4.3 无 resize：用 copy 比例缩放）
        side = int(max(best.w(), best.h()) * 1.5)
        x0 = max(0, cx - side // 2)
        y0 = max(0, cy - side // 2)
        x1 = min(img.width(), x0 + side)
        y1 = min(img.height(), y0 + side)
        roi = img.copy(roi=(x0, y0, x1 - x0, y1 - y0))
        sc = 32.0 / max(roi.width(), roi.height())
        roi32 = roi.copy(scale_x=sc, scale_y=sc)

        # 分类
        gid = 0xFF      # 低置信度/无目标（协议 0x03 定义 0xFF）
        conf = 0.0
        for obj in tf.classify(net, roi32):
            out = sorted(zip(labels, obj.output()), key=lambda x: x[1], reverse=True)
            conf = out[0][1]
            if conf >= CONF_MIN:
                gid = labels.index(out[0][0])
            label = out[0][0] if conf >= CONF_MIN else "?"
            print("[AI] %s = %.0f%% fps=%.1f" % (label, conf * 100, clock.fps()))
            if uart:
                uart.write(pack_gesture_ai(gid, int(conf * 100)))
        img.draw_rectangle(best.rect(), color=(255, 0, 0))
        img.draw_cross(cx, cy, color=(0, 255, 0))
        if uart:
            uart.write(pack_hand(True, cx, cy, best.w(), best.h()))
    else:
        ex_prev, ey_prev = None, None
        if uart:
            uart.write(pack_hand(False, 0, 0, 0, 0))
            uart.write(pack_gesture_ai(0xFF, 0))
