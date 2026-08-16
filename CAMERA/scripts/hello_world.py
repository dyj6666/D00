# -*- coding: utf-8 -*-
# Hello World —— OpenART mini 摄像头实时图像显示
# 用法：OpenMV IDE 打开本文件 → 点连接 → 点运行(绿色播放按钮)
#       左侧 Frame Buffer 窗口即实时显示画面，左上角叠加帧率。

import sensor
import image
import time

sensor.reset()                     # 复位摄像头
sensor.set_pixformat(sensor.RGB565)  # 彩色画面；追求速度可改 GRAYSCALE(灰度)
sensor.set_framesize(sensor.QVGA)    # 320x240；QQVGA(160x120) 帧率更高
sensor.skip_frames(time=500)         # 跳过前 500ms 帧，等曝光稳定

clock = time.clock()
while True:
    clock.tick()
    img = sensor.snapshot()          # 抓取一帧 —— IDE 左侧窗口自动显示
    # 在画面上叠加帧率文字（可选）
    img.draw_string(5, 5, "%.1f fps" % clock.fps(), color=(255, 255, 0))

    # 更多显示玩法（取消注释体验）：
    # img.draw_cross(160, 120, color=(255, 0, 0))   # 画面中心十字
    # img.draw_rectangle(60, 40, 120, 80, color=(0, 255, 0))  # 画框
