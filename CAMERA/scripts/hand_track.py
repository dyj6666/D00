# -*- coding: utf-8 -*-
# hand_track.py —— 手部追踪 + UART 协议输出（体感游戏 OpenART 侧）
# 玩法基础：手在镜头前左右移动 → 输出坐标 → MCU 屏幕角色跟随
#
# 验证（无需 MCU）：
#   IDE 打开本脚本 → 运行 → 手在镜头前挥动，串口终端显示：
#     [HAND] x=.. y=.. w=.. h=..   （30fps 稳定跟随 = 通过）
#   同时 UART2(B12/B13 @115200) 按 docs/串口协议.md 输出二进制帧（备 MCU 对接）
#
# 手/脸分离策略：手部操作区 = 画面下半部（y>45%），脸在上半部自然排除；
#   块过滤：尺寸/宽高比（排除长条手臂）；多块取面积最大（手离镜头近通常最大）
import sensor
import image
import time
from machine import UART

# ---------- 配置 ----------
THRESHOLDS = [(35, 88, 0, 30, 5, 35)]   # LAB 肤色（沿用已校准参数）
PIX_TH, AREA_TH = 120, 120
MIN_SIZE, MAX_SIZE = 25, 220             # 手部尺寸范围（QVGA）
RATIO_MIN, RATIO_MAX = 0.5, 1.6          # 近方形（拳头/手掌）
HAND_Y0 = 0.45                           # 手部区域：画面下半部起始比例
EMA_ALPHA = 0.6                          # 平滑系数（越大越跟手，越小越稳）

# ---------- UART 初始化（失败不影响 print 验证）----------
uart = None
try:
    uart = UART(2, baudrate=115200)      # OpenART mini: UART2 = TX:B12 RX:B13
    print("UART2 ready (B12/B13 @115200)")
except Exception as e:
    print("UART2 init skipped:", e)

# ---------- 协议打包（docs/串口协议.md 0x01 帧）----------
def pack_hand(detected, x, y, w, h):
    data = bytes([1 if detected else 0,
                  x & 0xFF, (x >> 8) & 0xFF,
                  y & 0xFF, (y >> 8) & 0xFF,
                  w & 0xFF, (w >> 8) & 0xFF,
                  h & 0xFF, (h >> 8) & 0xFF])
    s = 0x01
    for b in data:
        s = (s + b) & 0xFF
    return b'\xAA\x55' + bytes([0x01, len(data)]) + data + bytes([s])

# ---------- 主循环 ----------
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

W, H = sensor.width(), sensor.height()
roi_y0 = int(H * HAND_Y0)
ex_prev, ey_prev = None, None

clock = time.clock()
print("READY: 手放到画面下半部挥动")
while True:
    clock.tick()
    img = sensor.snapshot()

    blobs = img.find_blobs(THRESHOLDS, pixels_threshold=PIX_TH,
                           area_threshold=AREA_TH, merge=True)

    best = None
    best_area = 0
    for b in blobs:
        w, h = b.w(), b.h()
        if not (MIN_SIZE <= w <= MAX_SIZE and MIN_SIZE <= h <= MAX_SIZE):
            continue
        if not (RATIO_MIN < w / h < RATIO_MAX):
            continue
        if b.cy() < roi_y0:              # 只认下半部（排除脸）
            continue
        if b.area() > best_area:
            best = b
            best_area = b.area()

    if best:
        cx, cy = best.cx(), best.cy()
        # EMA 平滑
        if ex_prev is not None:
            cx = int(ex_prev * (1 - EMA_ALPHA) + cx * EMA_ALPHA)
            cy = int(ey_prev * (1 - EMA_ALPHA) + cy * EMA_ALPHA)
        ex_prev, ey_prev = cx, cy

        img.draw_rectangle(best.rect(), color=(255, 0, 0))
        img.draw_cross(cx, cy, color=(0, 255, 0))
        print("[HAND] x=%d y=%d w=%d h=%d fps=%.1f" %
              (cx, cy, best.w(), best.h(), clock.fps()))
        if uart:
            uart.write(pack_hand(True, cx, cy, best.w(), best.h()))
    else:
        ex_prev, ey_prev = None, None
        if uart:
            uart.write(pack_hand(False, 0, 0, 0, 0))
        if int(clock.fps()) % 30 == 0:
            print("no hand ... fps=%.1f" % clock.fps())
