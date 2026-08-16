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
MIN_SIZE, MAX_SIZE = 20, 320            # 上限放宽到全幅（胳膊伸直也放行）
RATIO_MIN, RATIO_MAX = 0.4, 2.5         # 宽高比放宽（长条手臂+手也能通过）
HAND_MAX_SIZE = 150                     # 端部裁剪边长上限（聚焦手，防整臂入框）
IS_HAND_AREA = 60000                    # 手级块面积上限（黑色桌面无身体干扰）
TRACK_DIST = 300        # 跟踪匹配距离（px），超过则重新选最大块
EMA_ALPHA = 0.6
MODEL = "/sd/model_gesture.tflite"
LABELS = "/sd/labels_gesture.txt"
CONF_MIN = 0.65         # 置信度低于此值输出 "?"
VOTE_N = 7              # 分类结果多数投票帧数（抑制帧间抖动）
SWITCH_CONFIRM = 3      # 新手势需连续 N 帧占优才切换（误判彻底压制）

# ---------- 双模式互斥（运动=挥手检测 / 静止=手势识别）----------
STILL_DISP = 12         # 帧间位移 < 此值视为"静止候选"（px）
STILL_CONFIRM_MS = 600  # 静止持续 ≥ 此时间才切到手势识别模式
MOVE_DISP = 20          # 帧间位移 ≥ 此值立即切回挥手检测模式
MODE_MOTION, MODE_STILL = 0, 1

# ---------- 挥手检测（窗口范围法：对跟踪丢帧鲁棒，仅运动模式）----------
# "一挥手" → SWIPE_LEFT/RIGHT 事件（协议 0x02）→ MCU 翻页
SWIPE_WINDOW = 6        # 坐标历史帧数（~0.2s）
SWIPE_RANGE = 110       # 窗口内位移范围 ≥ 此值触发事件（px）
SWIPE_COOLDOWN_MS = 700 # 冷却防连发
G_NONE, G_LEFT, G_RIGHT = 0x00, 0x01, 0x02
G_UP, G_DOWN = 0x03, 0x04

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
prev_size = 0
last_frame_t = 0
vote_buf = []
out_gid = -1            # 当前输出手势（切换确认状态）
switch_pend = 0
# 模式与挥手检测状态
mode = MODE_MOTION      # 当前模式：运动(挥手) / 静止(手势)
still_since = 0         # 静止候选起始时刻
prev_pos = None         # 上一帧原始坐标（帧间位移）
sw_hist = []            # 原始坐标历史（最多 SWIPE_WINDOW 帧）
sw_cooldown = 0
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
            if d > 300:
                best = max(cands, key=lambda b: b.area())  # 目标丢失：重新选最大
        else:
            best = max(cands, key=lambda b: b.area())

    # 手位置记忆：手独立时更新位置/大小；超大块时用记忆（黑色桌面下罕见）
    use_mem = False
    if best is not None:
        is_hand = best.area() < IS_HAND_AREA and max(best.w(), best.h()) < 300
        if is_hand:
            ex_prev, ey_prev = best.cx(), best.cy()
            prev_size = max(best.w(), best.h())
        elif ex_prev is not None and prev_size > 0:
            use_mem = True
        else:
            ex_prev, ey_prev = best.cx(), best.cy()
            prev_size = max(best.w(), best.h())
    else:
        if ex_prev is not None and (time.ticks_ms() - last_frame_t) > 500:
            ex_prev = ey_prev = None
            prev_size = 0

    if use_mem:
        # 身体级（手连通）：用记忆位置裁剪
        cx, cy = ex_prev, ey_prev
        side = int(prev_size * 1.1)
        track_ok = True
        cx_raw, cy_raw = cx, cy
    elif best is not None:
        # 手级块：EMA 平滑 + 端部定位（原始坐标保留给挥手检测用）
        cx_raw, cy_raw = best.cx(), best.cy()
        cx, cy = cx_raw, cy_raw
        if ex_prev is not None:
            cx = int(ex_prev * (1 - EMA_ALPHA) + cx * EMA_ALPHA)
            cy = int(ey_prev * (1 - EMA_ALPHA) + cy * EMA_ALPHA)
        bw, bh = best.w(), best.h()
        box_cx = best.x() + bw / 2.0
        box_cy = best.y() + bh / 2.0
        dx = best.cx() - box_cx
        dy = best.cy() - box_cy
        cx = int(best.cx() + dx * 0.6)
        cy = int(best.cy() + dy * 0.6)
        side = min(int(max(bw, bh) * 0.9), HAND_MAX_SIZE)
        track_ok = True
    else:
        cx = cy = side = 0
        cx_raw = cy_raw = 0
        track_ok = False

    last_frame_t = time.ticks_ms()

    # ---------- 双模式互斥状态机 ----------
    # 运动模式：只做挥手检测；静止模式：只做手势识别
    now2 = time.ticks_ms()
    if track_ok:
        if prev_pos is not None:
            disp = abs(cx_raw - prev_pos[0]) + abs(cy_raw - prev_pos[1])
        else:
            disp = 0
        prev_pos = (cx_raw, cy_raw)

        if mode == MODE_MOTION:
            # --- 挥手检测（窗口范围法）---
            sw_hist.append((cx_raw, cy_raw))
            if len(sw_hist) > SWIPE_WINDOW:
                sw_hist.pop(0)
            if len(sw_hist) >= 4:
                xs = [p[0] for p in sw_hist]
                ys = [p[1] for p in sw_hist]
                rx = max(xs) - min(xs)
                ry = max(ys) - min(ys)
                if max(rx, ry) >= SWIPE_RANGE and \
                   (now2 - sw_cooldown) >= SWIPE_COOLDOWN_MS:
                    # 方向：窗口首→尾净位移（横向取反补偿镜像）
                    dxs = -(sw_hist[-1][0] - sw_hist[0][0])
                    dys = sw_hist[-1][1] - sw_hist[0][1]
                    if abs(dxs) >= abs(dys):
                        sg = G_RIGHT if dxs > 0 else G_LEFT
                    else:
                        sg = G_DOWN if dys > 0 else G_UP
                    names = {G_LEFT: "SWIPE_LEFT", G_RIGHT: "SWIPE_RIGHT",
                             G_UP: "SWIPE_UP", G_DOWN: "SWIPE_DOWN"}
                    print("[GESTURE] %s" % names[sg])
                    if uart:
                        uart.write(pack(0x02, bytes([sg])))
                    sw_cooldown = now2
            # 静止确认：位移持续小 → 切手势模式
            if disp < STILL_DISP:
                if still_since == 0:
                    still_since = now2
                elif (now2 - still_since) >= STILL_CONFIRM_MS:
                    mode = MODE_STILL
                    still_since = 0
                    sw_hist.clear()
                    vote_buf.clear()
                    switch_pend = 0
                    print("[MODE] STILL 手势识别")
            else:
                still_since = 0
        else:
            # MODE_STILL：位移大立即切回运动模式
            if disp >= MOVE_DISP:
                mode = MODE_MOTION
                still_since = 0
                sw_hist.clear()
                print("[MODE] MOTION 挥手检测")
    else:
        # 手丢失：回运动模式等待
        mode = MODE_MOTION
        still_since = 0
        prev_pos = None
        sw_hist.clear()

    # ---------- 裁剪 + 分类（仅静止模式做 AI 识别）----------
    if track_ok:
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

        # 分类（仅静止模式；运动模式跳过——双模式互斥）
        gid = 0xFF      # 低置信度/无目标（协议 0x03 定义 0xFF）
        conf = 0.0
        if mode == MODE_STILL:
            for obj in tf.classify(net, roi32):
                out = sorted(zip(labels, obj.output()), key=lambda x: x[1], reverse=True)
                conf = out[0][1]
                if conf >= CONF_MIN:
                    gid = labels.index(out[0][0])
            vote_buf.append(gid if gid != 0xFF else -1)
            if len(vote_buf) > VOTE_N:
                vote_buf.pop(0)
            # 众数
            vgid = -1
            vconf = 0.0
            if vote_buf:
                vgid = max(set(vote_buf), key=vote_buf.count)
                vconf = sum(1 for v in vote_buf if v == vgid) * 100.0 / len(vote_buf)
            # 切换确认：新手势需连续 SWITCH_CONFIRM 帧占优才切换输出
            # （偶发误判无法累积到确认阈值 → 几乎不可能误输出）
            if vgid == out_gid:
                switch_pend = 0
            else:
                switch_pend += 1
                if switch_pend >= SWITCH_CONFIRM:
                    out_gid = vgid
                    switch_pend = 0
            if out_gid >= 0:
                label = labels[out_gid]
                gid = out_gid
                conf = vconf / 100.0
            else:
                label = "?"
                gid = 0xFF
            print("[AI] %s = %.0f%% fps=%.1f" % (label, conf * 100, clock.fps()))
            if uart:
                uart.write(pack_gesture_ai(gid, int(conf * 100)))
        # else: 运动模式——不发 0x03（挥手检测独占）
        # 画框：手级画 blob 框；记忆模式画记忆位置框
        if not use_mem and best is not None:
            img.draw_rectangle(best.rect(), color=(255, 0, 0))
        else:
            img.draw_rectangle((x0, y0, x1 - x0, y1 - y0), color=(255, 128, 0))
        img.draw_cross(cx, cy, color=(0, 255, 0))
        if uart:
            uart.write(pack_hand(True, cx, cy, x1 - x0, y1 - y0))
    else:
        ex_prev, ey_prev = None, None
        prev_size = 0
        vote_buf.clear()
        if uart:
            uart.write(pack_hand(False, 0, 0, 0, 0))
            uart.write(pack_gesture_ai(0xFF, 0))
