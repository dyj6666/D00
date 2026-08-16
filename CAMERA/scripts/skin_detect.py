# -*- coding: utf-8 -*-
# skin_detect.py —— 人脸区域检测（LAB 肤色方案，替代失效的 Haar）
# 原理：人脸皮肤在 LAB 色彩空间有特征区间，find_blobs 提取肤色连通域，
#       再用宽高比/面积过滤得到"人脸候选区域"。
# 本版同时打印每个色块的 LAB 统计，用于校准阈值（把脸/手放镜头前跑几秒）。
import sensor
import image
import time

sensor.reset()
sensor.set_pixformat(sensor.RGB565)   # 肤色检测需要彩色→LAB
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

# LAB 肤色阈值（L, A, B 各 (min,max)）—— 典型黄种人肤色区间，先跑校准
# 若检不到/误检多，看终端打印的 LAB 统计再调
THRESHOLDS = [(40, 90, 5, 30, 10, 45)]

clock = time.clock()
print("READY: 把脸/手放到镜头前，观察输出")
while True:
    clock.tick()
    img = sensor.snapshot()

    blobs = img.find_blobs(THRESHOLDS, pixels_threshold=150,
                           area_threshold=150, merge=True)

    for b in blobs:
        # 打印 LAB 统计（校准用）：每 30 帧一次避免刷屏
        if b.area() > 300 and int(clock.fps()) % 30 == 0:
            st = img.get_statistics(roi=b.rect())
            print("blob LAB: L[%d-%d] A[%d-%d] B[%d-%d] area=%d" %
                  (st.l_min(), st.l_max(), st.a_min(), st.a_max(),
                   st.b_min(), st.b_max(), b.area()))

        # 人脸候选过滤：尺寸足够大 + 宽高比接近方形
        if b.w() > 40 and b.h() > 40 and 0.5 < (b.w() / b.h()) < 1.8:
            img.draw_rectangle(b.rect(), color=(255, 0, 0))
            img.draw_cross(b.cx(), b.cy(), color=(0, 255, 0))
            print("FACE-REGION x=%d y=%d w=%d h=%d fps=%.1f" %
                  (b.x(), b.y(), b.w(), b.h(), clock.fps()))
