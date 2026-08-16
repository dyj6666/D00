# -*- coding: utf-8 -*-
# face_detection.py —— 人脸检测（Haar 级联，V4.3 固件标准能力）
# 用法：OpenMV IDE 打开 → 连接 → 运行（F5）
#   将人脸正对镜头（保持 0.3~1.5m 距离、光线充足），检测到的人脸画绿色框
# 说明：
#   - Haar 检测仅支持灰度图；QVGA 分辨率兼顾检测距离与帧率
#   - 这不是"认出是谁"（身份识别），仅检测"有没有人脸+位置"；
#     身份识别见 face_recognition 方案（需先录入人脸样本）
import sensor
import image
import time

sensor.reset()
sensor.set_contrast(3)            # 增强对比度，利于 Haar 检测
sensor.set_gainceiling(16)        # 增益上限
sensor.set_framesize(sensor.QVGA) # 320x240（OpenART mini 硬件上限）
sensor.set_pixformat(sensor.GRAYSCALE)
sensor.skip_frames(time=1000)

# 内置 frontalface Haar 级联（25 级；级数少更快但误检多）
face_cascade = image.HaarCascade("frontalface", stages=25)
print("cascade:", face_cascade)

clock = time.clock()
while True:
    clock.tick()
    img = sensor.snapshot()

    # 找所有人脸：threshold 越高检出率越高（误检也越多）
    objects = img.find_features(face_cascade, threshold=0.75, scale_factor=1.25)

    for r in objects:
        img.draw_rectangle(r, color=255)          # 白框标出人脸
        img.draw_cross(r[0] + r[2] // 2, r[1] + r[3] // 2, color=255)  # 中心十字
        print("face at x=%d y=%d w=%d h=%d" % (r[0], r[1], r[2], r[3]))

    print("fps:", clock.fps())
