# -*- coding: utf-8 -*-
# face_track.py —— 人脸区域跟踪（肤色方案实用版）
# 在 skin_detect 基础上增加工程过滤，抑制手/手臂误检：
#   1. ROI：只认画面中央偏上区域的肤色块（脸通常在此；手伸入画面会被排除）
#   2. 尺寸：w/h ∈ [40,220]（QVGA 下的合理脸尺寸），面积上限
#   3. 宽高比：0.6~1.4（收紧，排除长条手臂）
#   4. 多块时取面积最大者（脸通常是最大肤色块）
#   5. 输出：中心坐标 + 相对画面中心偏差(dx,dy)（云台/舵机追踪用）+ EMA 平滑
# 说明：肤色方案检测的是"肤色区域"而非语义"人脸"；手/手臂在 ROI 外
#       或尺寸不合规时被过滤，但无法 100% 消除（要语义需 AI 模型/固件升级）
import sensor
import image
import time

sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)      # 320x240
sensor.skip_frames(time=1000)

# LAB 肤色阈值（已按实测校准：L[23-86] A[-7-27] B[-10-29] 偏宽，收紧取核心）
THRESHOLDS = [(35, 88, 0, 30, 5, 35)]
PIX_TH = 150       # 最小像素数
AREA_TH = 150
# 过滤参数（QVGA 320x240）
MIN_SIZE = 40      # 最小边长 px
MAX_SIZE = 220     # 最大边长 px
MIN_AREA = 2000    # 最小面积 px²
MAX_AREA = 30000   # 最大面积 px²
RATIO_MIN = 0.6    # 宽高比下限（排除长条手臂）
RATIO_MAX = 1.4
ROI_X0, ROI_X1 = 0.15, 0.85   # 中央区域 x 范围（比例）
ROI_Y0, ROI_Y1 = 0.08, 0.72   # 中央偏上 y 范围（脸一般不在画面底部）

W = sensor.width()
H = sensor.height()
s_roi_x0 = int(W * ROI_X0); s_roi_x1 = int(W * ROI_X1)
s_roi_y0 = int(H * ROI_Y0); s_roi_y1 = int(H * ROI_Y1)
cx_prev, cy_prev = None, None

clock = time.clock()
print("READY: 脸放画面中央偏上区域")
while True:
    clock.tick()
    img = sensor.snapshot()

    blobs = img.find_blobs(THRESHOLDS, pixels_threshold=PIX_TH,
                           area_threshold=AREA_TH, merge=True)

    best = None
    best_area = 0
    for b in blobs:
        w, h = b.w(), b.h()
        if w < MIN_SIZE or h < MIN_SIZE or w > MAX_SIZE or h > MAX_SIZE:
            continue
        if b.area() < MIN_AREA or b.area() > MAX_AREA:
            continue
        if not (RATIO_MIN < (w / h) < RATIO_MAX):
            continue
        # ROI 过滤：blob 中心必须在中央偏上区域
        cx, cy = b.cx(), b.cy()
        if not (s_roi_x0 < cx < s_roi_x1 and s_roi_y0 < cy < s_roi_y1):
            continue
        # 选面积最大的候选
        if b.area() > best_area:
            best = b
            best_area = b.area()

    if best:
        cx, cy = best.cx(), best.cy()
        # EMA 平滑（0.6 新值权重，抑制抖动）
        if cx_prev is not None:
            cx = int(cx_prev * 0.4 + cx * 0.6)
            cy = int(cy_prev * 0.4 + cy * 0.6)
        cx_prev, cy_prev = cx, cy

        img.draw_rectangle(best.rect(), color=(255, 0, 0))
        img.draw_cross(cx, cy, color=(0, 255, 0))
        # 偏差：以画面中心为原点，右/下为正
        dx = cx - W // 2
        dy = cy - H // 2
        print("FACE cx=%d cy=%d dx=%+d dy=%+d w=%d h=%d fps=%.1f" %
              (cx, cy, dx, dy, best.w(), best.h(), clock.fps()))
    else:
        cx_prev, cy_prev = None, None
        if int(clock.fps()) % 30 == 0:
            print("no face ... fps=%.1f" % clock.fps())
