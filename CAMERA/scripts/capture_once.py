# -*- coding: utf-8 -*-
# capture_once.py —— 拍一帧图像保存到 SD 卡（/sd/photo.jpg）
# 注意：本脚本不依赖 LED/cmm 引脚映射（旧固件 + cmm_cfg.csv 版本不匹配
# 会报 "pinOBJ IS null"），拍照存卡仅用 sensor 即可。
import sensor
import image
import time

sensor.reset()
sensor.set_pixformat(sensor.RGB565)   # 彩色
sensor.set_framesize(sensor.QVGA)     # 320x240
sensor.skip_frames(time=500)          # 等曝光稳定

img = sensor.snapshot()               # 抓一帧
img.save("/sd/photo.jpg", quality=90) # 存到 SD 卡根目录
print("saved /sd/photo.jpg OK")
