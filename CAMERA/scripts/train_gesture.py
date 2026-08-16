# -*- coding: utf-8 -*-
# train_gesture.py —— 手势分类模型训练 v4（离线数据增强版）
# 输入：CAMERA/train/dataset/<CLASS>/*.jpg（OpenART 采集）
# 输出：CAMERA/train/model_gesture.tflite（float32）+ labels_gesture.txt
# v4 变化：PIL 离线增强（旋转/平移/缩放/翻转，仅训练集）——解决
#   手位置偏移/角度倾斜识别错误；tf.keras 增强层弃用（验证阶段异常）。
# 运行：CAMERA/train/.venv/Scripts/python.exe CAMERA/scripts/train_gesture.py
import os
import random
import numpy as np
from PIL import Image, ImageEnhance

import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers

DATASET = r"D:\GIT-SPACE\D00\CAMERA\train\dataset"
OUT_MODEL = r"D:\GIT-SPACE\D00\CAMERA\train\model_gesture.tflite"
OUT_LABELS = r"D:\GIT-SPACE\D00\CAMERA\train\labels_gesture.txt"
IMG_SIZE = 32
EPOCHS = 40
BATCH = 64
VAL_SPLIT = 0.15
AUG_PER_IMG = 13       # 每张训练图生成增强副本数（不含原图）

random.seed(42)
np.random.seed(42)


def rot_crop(img, ang):
    """旋转后中心裁剪回原尺寸（去黑边）——模拟手在画面中倾斜的真实效果"""
    w, h = img.size
    r = img.rotate(ang, resample=Image.BILINEAR, expand=True)
    rw, rh = r.size
    x0 = (rw - w) // 2
    y0 = (rh - h) // 2
    return r.crop((x0, y0, x0 + w, y0 + h))


def augment(img, size):
    """PIL 离线增强：旋转(去黑边)/平移/缩放/翻转/亮度，返回副本列表"""
    outs = []
    w, h = size
    # 1. 旋转 ±12 / ±25（去黑边）
    for ang in (-25, -12, 12, 25):
        outs.append(rot_crop(img, ang))
    # 2. 平移 ±15%
    for dx, dy in ((-int(w * 0.15), 0), (int(w * 0.15), 0),
                   (0, -int(h * 0.15)), (0, int(h * 0.15))):
        t = Image.new("RGB", (w, h), (0, 0, 0))
        t.paste(img, (dx, dy))
        outs.append(t)
    # 3. 缩放 0.75 / 1.3（居中）
    for s in (0.75, 1.3):
        nw, nh = max(1, int(w * s)), max(1, int(h * s))
        z = img.resize((nw, nh))
        t = Image.new("RGB", (w, h), (0, 0, 0))
        t.paste(z, ((w - nw) // 2, (h - nh) // 2))
        outs.append(t)
    # 4. 水平翻转 + 亮度变化
    outs.append(img.transpose(Image.FLIP_LEFT_RIGHT))
    outs.append(ImageEnhance.Brightness(img).enhance(0.85))
    outs.append(ImageEnhance.Brightness(img).enhance(1.15))
    return outs[:AUG_PER_IMG]


# ---------- 加载数据 ----------
classes = sorted([d for d in os.listdir(DATASET)
                  if os.path.isdir(os.path.join(DATASET, d))])
print("类别:", classes)

xs, ys, augs = [], [], []
for ci, c in enumerate(classes):
    files = [f for f in os.listdir(os.path.join(DATASET, c))
             if f.lower().endswith((".jpg", ".jpeg", ".png"))]
    for f in files:
        img = Image.open(os.path.join(DATASET, c, f)).convert("RGB")
        img = img.resize((IMG_SIZE, IMG_SIZE))
        xs.append(np.array(img, dtype=np.float32) / 255.0)
        ys.append(ci)
        augs.append(augment(img, (IMG_SIZE, IMG_SIZE)))
    print("  %s: %d 张" % (c, len(files)))

xs = np.array(xs)
ys = np.array(ys)
print("原始样本:", len(xs))

# ---------- 手动划分：验证集只用原图；训练集 = 原图 + 增强副本 ----------
idx = np.random.permutation(len(xs))
n_val = int(len(xs) * VAL_SPLIT)
val_idx, train_idx = idx[:n_val], idx[n_val:]

x_train, y_train = [], []
for i in train_idx:
    x_train.append(xs[i])
    y_train.append(ys[i])
    for a in augs[i]:
        x_train.append(np.array(a, dtype=np.float32) / 255.0)
        y_train.append(ys[i])
x_train = np.array(x_train)
y_train = np.array(y_train)
x_val = xs[val_idx]
y_val = ys[val_idx]
print("训练集(含增强):", len(x_train), " 验证集:", len(x_val))

# ---------- 模型（轻量 CNN）----------
model = keras.Sequential([
    keras.Input(shape=(IMG_SIZE, IMG_SIZE, 3)),
    layers.Conv2D(16, 3, activation="relu", padding="same"),
    layers.MaxPooling2D(),
    layers.Conv2D(32, 3, activation="relu", padding="same"),
    layers.MaxPooling2D(),
    layers.Conv2D(64, 3, activation="relu", padding="same"),
    layers.MaxPooling2D(),
    layers.Flatten(),
    layers.Dropout(0.4),
    layers.Dense(64, activation="relu"),
    layers.Dense(len(classes), activation="softmax"),
])
model.compile(optimizer="adam", loss="sparse_categorical_crossentropy",
              metrics=["accuracy"])

model.fit(x_train, y_train, epochs=EPOCHS, batch_size=BATCH,
          validation_data=(x_val, y_val), verbose=1)
loss, acc = model.evaluate(x_val, y_val, verbose=0)
print("训练完成: val_acc=%.3f" % acc)

# ---------- 导出 tflite ----------
converter = tf.lite.TFLiteConverter.from_keras_model(model)
tflite_model = converter.convert()
with open(OUT_MODEL, "wb") as f:
    f.write(tflite_model)
with open(OUT_LABELS, "w") as f:
    f.write("\n".join(classes) + "\n")
print("模型已导出:", OUT_MODEL, os.path.getsize(OUT_MODEL), "B")

# ---------- 自检 ----------
interp = tf.lite.Interpreter(model_path=OUT_MODEL)
interp.allocate_tensors()
in_d = interp.get_input_details()[0]
out_d = interp.get_output_details()[0]
ok = 0
for i in range(min(60, len(x_val))):
    interp.set_tensor(in_d["index"], x_val[i:i + 1])
    interp.invoke()
    if int(np.argmax(interp.get_tensor(out_d["index"])[0])) == y_val[i]:
        ok += 1
print("tflite 验证集自检: %d/%d 正确" % (ok, min(60, len(x_val))))
