# -*- coding: utf-8 -*-
# face_detection_x2.py —— 人脸检测（2x 放大预处理版，QVGA）
# 为什么放大：Haar 检测金字塔只会"向下缩"，130° 广角下小脸检不出；
# 先把图像放大 2 倍再检测，等效把小脸放大成标准脸。坐标已换算回原图。
import sensor
import image
import time

sensor.reset()
sensor.set_contrast(3)
sensor.set_gainceiling(16)
sensor.set_framesize(sensor.QVGA)      # 320x240（OpenART 稳定分辨率）
sensor.set_pixformat(sensor.GRAYSCALE)
sensor.skip_frames(time=1000)

face_cascade = image.HaarCascade("frontalface", stages=25)
print("CASCADE OK:", face_cascade)

clock = time.clock()
print("READY, 请把正脸靠近镜头 0.3~0.6m")
while True:
    clock.tick()
    img = sensor.snapshot()

    # 放大 2 倍（copy 分配在 SDRAM 堆，QVGA->640x480 灰度约 300KB，安全）
    big = img.copy(scale_x=2, scale_y=2)

    objs = big.find_features(face_cascade, threshold=0.6, scale_factor=1.25)
    th_used = 0.6
    if not objs:
        objs = big.find_features(face_cascade, threshold=0.35, scale_factor=1.25)
        th_used = 0.35

    for r in objs:
        x = r[0] // 2
        y = r[1] // 2
        w = r[2] // 2
        h = r[3] // 2
        img.draw_rectangle((x, y, w, h), color=255)
        img.draw_cross(x + w // 2, y + h // 2, color=255)
        print("FACE th=%.2f x=%d y=%d w=%d h=%d fps=%.1f" % (th_used, x, y, w, h, clock.fps()))
