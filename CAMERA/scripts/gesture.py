# -*- coding: utf-8 -*-
# gesture.py —— 手势识别引擎（事件级，零训练，V4.3 固件）
# 基于肤色手部追踪，输出离散手势事件（docs/串口协议.md 0x02 帧）：
#   SWIPE_LEFT/RIGHT/UP/DOWN  快速挥动（体感翻页/移动）
#   GRAB                      捏合/抓取（面积快速变化）
#   HOVER                     悬停 ≥0.8s（确认/点击，持续期每500ms重发）
# 同时每帧输出坐标帧(0x01)——MCU 侧可同步获得连续位置用于角色跟随。
#
# 验证（无需 MCU）：IDE 运行 → 挥手/抓取/悬停 → 终端显示 [GESTURE] 事件
import sensor
import image
import time
from machine import UART

# ---------- 配置 ----------
THRESHOLDS = [(35, 88, 0, 30, 5, 35)]
PIX_TH, AREA_TH = 120, 120
MIN_SIZE, MAX_SIZE = 25, 220
RATIO_MIN, RATIO_MAX = 0.5, 1.6
HAND_Y0 = 0.45
EMA_ALPHA = 0.6

SWIPE_DIST = 110        # 0.4s 内累计位移 ≥ 此值判定挥动（px）
SWIPE_WIN_S = 0.4       # 挥动判定窗口（s）
GRAB_CHANGE = 0.45      # 0.3s 内面积变化 ≥ 45% 判定抓取
HOVER_STABLE = 15       # 悬停抖动阈值（px）
HOVER_TIME_S = 0.8      # 悬停判定时间（s）
HOVER_REPEAT_MS = 500   # 悬停重发间隔
EVENT_COOLDOWN_MS = 300 # 事件冷却（防连发）

# 手势 ID（协议 0x02）
G_NONE, G_LEFT, G_RIGHT = 0x00, 0x01, 0x02
G_UP, G_DOWN, G_GRAB, G_HOVER = 0x03, 0x04, 0x05, 0x06

# ---------- UART ----------
uart = None
try:
    uart = UART(2, baudrate=115200)
    print("UART2 ready (B12/B13 @115200)")
except Exception as e:
    print("UART2 init skipped:", e)

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

def pack_gesture(gid):
    return pack(0x02, bytes([gid]))

# ---------- 手势状态机 ----------
class GestureEngine:
    def __init__(self):
        self.hx, self.hy = None, None          # 平滑坐标
        self.harea = 0
        self.last_sw_t = 0                     # 上次挥动判定时刻
        self.sw_x0, self.sw_y0 = 0, 0          # 挥动窗口起点
        self.sw_t0 = 0
        self.grab_area0, self.grab_t0 = 0, 0   # 抓取窗口
        self.hover_t0 = 0
        self.hover_sent_t = 0
        self.cooldown_t = 0
        self.present = False

    def update(self, now, cx, cy, area, present):
        out = G_NONE
        if present:
            # --- 挥动：窗口内累计位移 ---
            if not self.present:
                self.sw_x0, self.sw_y0 = cx, cy
                self.sw_t0 = now
            dt = now - self.sw_t0
            if dt >= SWIPE_WIN_S:
                dx = cx - self.sw_x0
                dy = cy - self.sw_y0
                if abs(dx) >= SWIPE_DIST or abs(dy) >= SWIPE_DIST:
                    if now - self.cooldown_t >= EVENT_COOLDOWN_MS:
                        if abs(dx) >= abs(dy):
                            out = G_RIGHT if dx > 0 else G_LEFT
                        else:
                            out = G_DOWN if dy > 0 else G_UP
                        self.cooldown_t = now
                self.sw_x0, self.sw_y0 = cx, cy
                self.sw_t0 = now

            # --- 抓取：面积快速变化 ---
            if self.harea > 0 and now - self.grab_t0 >= 0.3:
                change = abs(area - self.grab_area0) / max(self.grab_area0, 1)
                if change >= GRAB_CHANGE and now - self.cooldown_t >= EVENT_COOLDOWN_MS:
                    out = G_GRAB
                    self.cooldown_t = now
                self.grab_area0, self.grab_t0 = area, now
            elif self.harea == 0:
                self.grab_area0, self.grab_t0 = area, now

            # --- 悬停：位置稳定 ---
            if self.hx is not None:
                if abs(cx - self.hx) < HOVER_STABLE and abs(cy - self.hy) < HOVER_STABLE:
                    if self.hover_t0 == 0:
                        self.hover_t0 = now
                    elif (now - self.hover_t0) >= HOVER_TIME_S:
                        if now - self.hover_sent_t >= HOVER_REPEAT_MS:
                            out = G_HOVER
                            self.hover_sent_t = now
                else:
                    self.hover_t0 = 0
            else:
                self.hover_t0 = 0

            self.hx, self.hy = cx, cy
            self.harea = area
        else:
            self.hx, self.hy = None, None
            self.harea = 0
            self.hover_t0 = 0
            self.sw_t0 = 0

        self.present = present
        return out


# ---------- 主循环 ----------
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

W, H = sensor.width(), sensor.height()
roi_y0 = int(H * HAND_Y0)
engine = GestureEngine()

clock = time.clock()
last_t = time.ticks_ms()
print("READY: 挥手/抓取/悬停试试")
while True:
    clock.tick()
    now = time.ticks_ms()
    img = sensor.snapshot()

    blobs = img.find_blobs(THRESHOLDS, pixels_threshold=PIX_TH,
                           area_threshold=AREA_TH, merge=True)
    best, best_area = None, 0
    for b in blobs:
        w, h = b.w(), b.h()
        if not (MIN_SIZE <= w <= MAX_SIZE and MIN_SIZE <= h <= MAX_SIZE):
            continue
        if not (RATIO_MIN < w / h < RATIO_MAX):
            continue
        if b.cy() < roi_y0:
            continue
        if b.area() > best_area:
            best, best_area = b, b.area()

    present = best is not None
    if present:
        cx, cy = best.cx(), best.cy()
        if engine.hx is not None:
            cx = int(engine.hx * (1 - EMA_ALPHA) + cx * EMA_ALPHA)
            cy = int(engine.hy * (1 - EMA_ALPHA) + cy * EMA_ALPHA)
        img.draw_rectangle(best.rect(), color=(255, 0, 0))
        img.draw_cross(cx, cy, color=(0, 255, 0))
    else:
        cx = cy = 0

    gid = engine.update(now, cx, cy, best_area, present)

    if uart:
        uart.write(pack_hand(present, cx, cy, best.w() if best else 0,
                             best.h() if best else 0))
        if gid != G_NONE:
            uart.write(pack_gesture(gid))

    if gid != G_NONE:
        names = {G_LEFT: "SWIPE_LEFT", G_RIGHT: "SWIPE_RIGHT",
                 G_UP: "SWIPE_UP", G_DOWN: "SWIPE_DOWN",
                 G_GRAB: "GRAB", G_HOVER: "HOVER"}
        print("[GESTURE] %s" % names[gid])
    elif present and int(clock.fps()) % 30 == 0:
        print("[HAND] x=%d y=%d w=%d h=%d fps=%.1f" %
              (cx, cy, best.w(), best.h(), clock.fps()))
