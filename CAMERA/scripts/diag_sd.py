# -*- coding: utf-8 -*-
# diag_sd.py —— 诊断 SD 卡挂载与写入
import os
import sensor
import image
import time

# 1. 看目录结构
print("listdir /  :", os.listdir("/"))
try:
    print("listdir /sd:", os.listdir("/sd"))
except Exception as e:
    print("listdir /sd FAIL:", e)

# 2. 拍照尝试写入多个候选路径
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QQVGA)
sensor.skip_frames(time=300)
img = sensor.snapshot()

for path in ("/sd/photo.jpg", "/photo.jpg"):
    try:
        img.save(path, quality=85)
        print("SAVE OK:", path)
    except Exception as e:
        print("SAVE FAIL:", path, "->", e)

# 3. 再看目录
try:
    print("after save, /sd:", os.listdir("/sd"))
except Exception as e:
    print("after save, /sd FAIL:", e)
print("diag done")
