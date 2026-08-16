# -*- coding: utf-8 -*-
# offline_face_test.py —— 离线人脸检测测试（BMP 加载版）
# OpenMV image.Image(path) 仅可靠支持 BMP 文件加载；JPEG 加载/绘制
# 在 OpenART 固件上有兼容问题。前置：face_test.bmp（320x240 灰度）放 TF 卡根目录
import sensor
import image
import time

sensor.reset()
sensor.set_pixformat(sensor.GRAYSCALE)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=200)

face_cascade = image.HaarCascade("frontalface", stages=25)

# 1. 帧缓冲图像（mutable）
img = sensor.snapshot()

# 2. 加载 BMP 并画到帧缓冲
try:
    off = image.Image("/sd/face_test.bmp")
    print("loaded bmp:", off.width(), "x", off.height())
    img.draw_image(off, 0, 0)
    print("overlay OK")
except Exception as e:
    print("FAIL:", e)
    raise SystemExit

# 3. 多阈值检测
for th in (0.75, 0.5, 0.3, 0.2):
    objs = img.find_features(face_cascade, threshold=th, scale_factor=1.25)
    print("th=%.2f -> %d face(s) %s" % (th, len(objs), objs[:2]))
    if objs:
        for r in objs:
            img.draw_rectangle(r, color=255)
        break

# 4. 放大 2 倍再检测
print("-- upscale x2 --")
big = img.copy(scale_x=2, scale_y=2)
for th in (0.5, 0.3):
    objs = big.find_features(face_cascade, threshold=th, scale_factor=1.25)
    print("th=%.2f -> %d face(s) %s" % (th, len(objs), objs[:2]))
    if objs:
        break

print("done")
