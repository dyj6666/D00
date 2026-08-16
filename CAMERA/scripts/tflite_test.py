# -*- coding: utf-8 -*-
# tflite_test.py —— 验证 OpenART V4.3 的 tf 模块（tflite AI 分类）
# 前置：model_18_0.7639_quant.tflite + labels_animal_fruits.txt 已在 TF 卡根目录
# 验证：把水果/动物图片（电脑屏幕/手机屏/打印）放到镜头前，看识别结果。
#       数据集图片在 CAMERA/dataset/紫色边框/（猫/狗/苹果/香蕉...）
import sensor
import image
import time
import tf

sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QQVGA)     # 例程规格：小分辨率保证内存
sensor.skip_frames(time=1000)

labels = [line.rstrip() for line in open("/sd/labels_animal_fruits.txt")]
print("labels:", labels)

net = tf.load("/sd/model_18_0.7639_quant.tflite", load_to_fb=True)
print("MODEL LOADED OK:", net)

clock = time.clock()
print("READY: 把猫/狗/水果图片放到镜头前")
while True:
    clock.tick()
    img = sensor.snapshot()

    for obj in tf.classify(net, img):
        out = sorted(zip(labels, obj.output()), key=lambda x: x[1], reverse=True)
        top = out[0]
        print("[AI] %s = %.2f%% fps=%.1f" % (top[0], top[1] * 100, clock.fps()))
        img.draw_string(5, 5, "%s %.0f%%" % (top[0], top[1] * 100),
                        color=(255, 255, 0))
