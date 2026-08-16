# -*- coding: utf-8 -*-
# train_gesture.py —— 手势分类模型训练（电脑端，TensorFlow）
# 输入：CAMERA/train/dataset/<CLASS>/*.jpg（OpenART 采集）
# 输出：CAMERA/train/model_gesture.tflite（float32）+ labels_gesture.txt
# 运行：CAMERA/train/.venv/Scripts/python.exe CAMERA/scripts/train_gesture.py
import os
import numpy as np
from PIL import Image

import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers

DATASET = r"D:\GIT-SPACE\D00\CAMERA\train\dataset"
OUT_MODEL = r"D:\GIT-SPACE\D00\CAMERA\train\model_gesture.tflite"
OUT_LABELS = r"D:\GIT-SPACE\D00\CAMERA\train\labels_gesture.txt"
IMG_SIZE = 32          # 模型输入（OpenMV tflite 兼容小尺寸）
EPOCHS = 40
BATCH = 32
VAL_SPLIT = 0.15

# ---------- 加载数据 ----------
classes = sorted([d for d in os.listdir(DATASET)
                  if os.path.isdir(os.path.join(DATASET, d))])
print("类别:", classes)
xs, ys = [], []
for ci, c in enumerate(classes):
    files = [f for f in os.listdir(os.path.join(DATASET, c))
             if f.lower().endswith((".jpg", ".jpeg", ".png"))]
    for f in files:
        img = Image.open(os.path.join(DATASET, c, f)).convert("RGB")
        img = img.resize((IMG_SIZE, IMG_SIZE))
        xs.append(np.array(img, dtype=np.float32) / 255.0)
        ys.append(ci)
    print("  %s: %d 张" % (c, len(files)))

xs = np.array(xs)
ys = np.array(ys)
print("总样本:", len(xs), "形状:", xs.shape)

# ---------- 数据增强 ----------
# 注：tf.keras 增强层在此环境验证阶段异常（val_acc=0）且转换 tflite 时
# 产生 Captures 警告——弃用。位置/大小鲁棒性改由"采集侧多样化"保证
# （collect_gestures.py 保存时随机偏移/缩放裁剪框）。
da = None

# ---------- 模型（轻量 CNN，OpenMV 可跑）----------
model = keras.Sequential([
    keras.Input(shape=(IMG_SIZE, IMG_SIZE, 3)),
    layers.Rescaling(1.0),  # 已归一化
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
model.summary()

# ---------- 训练 ----------
history = model.fit(xs, ys, epochs=EPOCHS, batch_size=BATCH,
                    validation_split=VAL_SPLIT, verbose=1)
acc = history.history["accuracy"][-1]
val_acc = history.history["val_accuracy"][-1]
print("训练完成: acc=%.3f val_acc=%.3f" % (acc, val_acc))

# ---------- 导出 tflite（float32，OpenMV 兼容）----------
converter = tf.lite.TFLiteConverter.from_keras_model(model)
tflite_model = converter.convert()
with open(OUT_MODEL, "wb") as f:
    f.write(tflite_model)
with open(OUT_LABELS, "w") as f:
    f.write("\n".join(classes) + "\n")
print("模型已导出:", OUT_MODEL, os.path.getsize(OUT_MODEL), "B")
print("标签已导出:", OUT_LABELS)

# ---------- 自检：训练集/验证集抽样推理 ----------
interp = tf.lite.Interpreter(model_path=OUT_MODEL)
interp.allocate_tensors()
in_d = interp.get_input_details()[0]
out_d = interp.get_output_details()[0]
ok = 0
for i in range(min(30, len(xs))):
    interp.set_tensor(in_d["index"], xs[i:i + 1])
    interp.invoke()
    pred = int(np.argmax(interp.get_tensor(out_d["index"])[0]))
    if pred == ys[i]:
        ok += 1
print("tflite 自检（前 30 张）: %d/30 正确" % ok)
