# -*- coding: utf-8 -*-
# gesture.py v2 —— 手势识别引擎（运动感知目标锁定版）
# 解决"脸被当手"：不再依赖"下半部"假设，改为运动感知锁定：
#   - 帧间匹配给每个肤色块建立连续 ID（位移<60px 视为同一块）
#   - 新出现的块（手伸入画面）优先锁定；锁定的块持续跟踪（手静止也保持）
#   - 静止的块（脸）移动量衰减为 0，永不抢锁；手离开 500ms 后解锁
# 手势事件不变：SWIPE_LEFT/RIGHT/UP/DOWN / GRAB / HOVER（协议 0x02）
import sensor
import image
import time
from machine import UART

# ---------- 配置 ----------
THRESHOLDS = [(35, 88, 0, 30, 5, 35)]
PIX_TH, AREA_TH = 120, 120
MIN_SIZE, MAX_SIZE = 25, 220
RATIO_MIN, RATIO_MAX = 0.5, 1.6
EMA_ALPHA = 0.6

MATCH_DIST = 60        # 帧间同块匹配距离（px）
MOTION_DECAY = 0.85    # 移动量历史衰减（静止块快速归零）
NEW_MOTION = 999.0     # 新块初始移动量（优先锁定）
UNLOCK_MS = 500        # 锁定块消失后解锁延时

SWIPE_DIST = 110
SWIPE_WIN_S = 0.4
GRAB_CHANGE = 0.45
HOVER_STABLE = 15
HOVER_TIME_S = 0.8
HOVER_REPEAT_MS = 500
EVENT_COOLDOWN_MS = 300

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

# ---------- 目标锁定跟踪 ----------
class TargetTracker:
    def __init__(self):
        self.tracks = []        # [{x,y,area,motion,age,last_seen}]
        self.locked = None      # 锁定块（对象引用）
        self.lock_lost_t = 0

    def update(self, cands, now):
        """cands: [(cx, cy, area)] 候选列表；返回选中 (cx,cy,area) 或 None"""
        # 1. 帧间匹配（贪心最近邻）
        matched = [False] * len(cands)
        for t in self.tracks:
            best_i, best_d = -1, MATCH_DIST
            for i, (cx, cy, a) in enumerate(cands):
                if matched[i]:
                    continue
                d = abs(t['x'] - cx) + abs(t['y'] - cy)
                if d < best_d:
                    best_d, best_i = d, i
            if best_i >= 0:
                cx, cy, a = cands[best_i]
                t['motion'] = t['motion'] * MOTION_DECAY + best_d
                t['x'], t['y'], t['area'] = cx, cy, a
                t['age'] += 1
                t['last_seen'] = now
                matched[best_i] = True

        # 2. 新块（未匹配候选 → 新目标，高移动量）
        for i, (cx, cy, a) in enumerate(cands):
            if not matched[i]:
                self.tracks.append({'x': cx, 'y': cy, 'area': a,
                                    'motion': NEW_MOTION, 'age': 1,
                                    'last_seen': now})

        # 3. 清理超时块（1s 未匹配）
        self.tracks = [t for t in self.tracks if now - t['last_seen'] < 1000]

        # 4. 选择：锁定优先
        if self.locked is not None:
            if self.locked in self.tracks and self.locked['last_seen'] == now:
                return (self.locked['x'], self.locked['y'], self.locked['area'])
            # 锁定块消失：计时解锁
            if self.lock_lost_t == 0:
                self.lock_lost_t = now
            elif now - self.lock_lost_t > UNLOCK_MS:
                self.locked = None
                self.lock_lost_t = 0
        else:
            # 5. 新锁定：移动量最大的块（手伸入=新块=999）
            if self.tracks:
                t = max(self.tracks, key=lambda t: t['motion'])
                self.locked = t
                return (t['x'], t['y'], t['area'])
        return None


# ---------- 手势状态机 ----------
class GestureEngine:
    def __init__(self):
        self.hx = self.hy = None
        self.harea = 0
        self.sw_x0 = self.sw_y0 = 0
        self.sw_t0 = 0
        self.grab_area0 = self.grab_t0 = 0
        self.hover_t0 = 0
        self.hover_sent_t = 0
        self.cooldown_t = 0
        self.present = False

    def update(self, now, cx, cy, area, present):
        out = G_NONE
        if present:
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

            if self.harea > 0 and now - self.grab_t0 >= 0.3:
                change = abs(area - self.grab_area0) / max(self.grab_area0, 1)
                if change >= GRAB_CHANGE and now - self.cooldown_t >= EVENT_COOLDOWN_MS:
                    out = G_GRAB
                    self.cooldown_t = now
                self.grab_area0, self.grab_t0 = area, now
            elif self.harea == 0:
                self.grab_area0, self.grab_t0 = area, now

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
            self.hx = self.hy = None
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

tracker = TargetTracker()
engine = GestureEngine()

clock = time.clock()
print("READY: 伸入的手会被锁定，静止的脸不会抢")
while True:
    clock.tick()
    now = time.ticks_ms()
    img = sensor.snapshot()

    blobs = img.find_blobs(THRESHOLDS, pixels_threshold=PIX_TH,
                           area_threshold=AREA_TH, merge=True)
    cands = []
    for b in blobs:
        w, h = b.w(), b.h()
        if not (MIN_SIZE <= w <= MAX_SIZE and MIN_SIZE <= h <= MAX_SIZE):
            continue
        if not (RATIO_MIN < w / h < RATIO_MAX):
            continue
        cands.append((b.cx(), b.cy(), b.area()))

    hit = tracker.update(cands, now)

    present = hit is not None
    bw = bh = 0
    if present:
        cx, cy, area = hit
        if engine.hx is not None:
            cx = int(engine.hx * (1 - EMA_ALPHA) + cx * EMA_ALPHA)
            cy = int(engine.hy * (1 - EMA_ALPHA) + cy * EMA_ALPHA)
        img.draw_cross(cx, cy, color=(0, 255, 0))
        # 画选中块框（用与坐标最近的 blob）
        best_b = None
        for b in blobs:
            if abs(b.cx() - cx) < 40 and abs(b.cy() - cy) < 40:
                best_b = b
                break
        if best_b:
            img.draw_rectangle(best_b.rect(), color=(255, 0, 0))
            bw, bh = best_b.w(), best_b.h()
    else:
        cx = cy = 0

    gid = engine.update(now, cx, cy, area if present else 0, present)

    if uart:
        uart.write(pack_hand(present, cx, cy, bw, bh))
        if gid != G_NONE:
            uart.write(pack_gesture(gid))

    if gid != G_NONE:
        names = {G_LEFT: "SWIPE_LEFT", G_RIGHT: "SWIPE_RIGHT",
                 G_UP: "SWIPE_UP", G_DOWN: "SWIPE_DOWN",
                 G_GRAB: "GRAB", G_HOVER: "HOVER"}
        print("[GESTURE] %s" % names[gid])
    elif present and int(clock.fps()) % 30 == 0:
        print("[HAND] x=%d y=%d w=%d h=%d fps=%.1f" %
              (cx, cy, bw, bh, clock.fps()))
