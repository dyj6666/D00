# -*- coding: utf-8 -*-
# face_diag.py —— 人脸检测诊断：图像状态 + 多阈值检测
# 运行后串口终端会打印诊断信息，把输出复制给我
import sensor
import image
import time

sensor.reset()
sensor.set_contrast(3)
sensor.set_gainceiling(16)
sensor.set_framesize(sensor.QVGA)
sensor.set_pixformat(sensor.GRAYSCALE)
sensor.skip_frames(time=1000)

# 1. 抓一帧看图像统计（判断过暗/过曝）
img = sensor.snapshot()
st = img.get_statistics()
print("== IMG STATS ==")
print("mean=%d min=%d max=%d" % (st.mean(), st.min(), st.max()))

# 2. 级联加载状态
face_cascade = image.HaarCascade("frontalface", stages=25)
print("== CASCADE ==", face_cascade)

clock = time.clock()
print("== DETECT LOOP (对镜头保持人脸不动 8 秒) ==")
t0 = time.ticks_ms()
frames = 0
found_total = 0
while time.ticks_ms() - t0 < 8000:
    clock.tick()
    img = sensor.snapshot()
    # 多档阈值尝试（从严格到宽松）
    hit = None
    for th in (0.75, 0.55, 0.35):
        objs = img.find_features(face_cascade, threshold=th, scale_factor=1.25)
        if objs:
            hit = (th, objs)
            break
    frames += 1
    if hit:
        found_total += 1
        th, objs = hit
        for r in objs:
            img.draw_rectangle(r, color=255)
        print("FOUND th=%.2f n=%d %s fps=%.1f" % (th, len(objs), objs[:1], clock.fps()))
    else:
        # 每 20 帧打印一次没找到
        if frames % 20 == 1:
            print("none ... fps=%.1f" % clock.fps())

print("== DONE ==")
print("frames=%d frames_with_face=%d" % (frames, found_total))
