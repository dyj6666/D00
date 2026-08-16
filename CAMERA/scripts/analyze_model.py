# -*- coding: utf-8 -*-
# analyze_model.py —— 电脑端 tflite 模型分析（验证 OpenART 恒出 cat 的原因）
# 用法：D:\Python\python.exe CAMERA/scripts/analyze_model.py
# 输出：模型输入/输出规格 + 数据集图片的推理结果（验证模型有效性与类别顺序）
import os
import numpy as np
from PIL import Image

MODEL = r"D:\GIT-SPACE\D00\CAMERA\train\model_18_0.7639_quant.tflite"
LABELS = ["cat", "dog", "horse", "pig", "casttle", "apple", "orange",
          "banana", "durian", "grape"]
DS = r"D:\GIT-SPACE\D00\CAMERA\dataset\紫色边框"
TEST = [("猫", "猫.png"), ("狗", "狗.png"), ("苹果", "苹果.png"),
        ("香蕉", "香蕉.png"), ("马", "马.png"), ("橙子", "橙子.png")]

import tensorflow as tf
interp = tf.lite.Interpreter(model_path=MODEL)
interp.allocate_tensors()
in_d = interp.get_input_details()[0]
out_d = interp.get_output_details()[0]
print("== 模型规格 ==")
print("input :", in_d["shape"], in_d["dtype"], "scale/zp:",
      in_d.get("quantization"))
print("output:", out_d["shape"], out_d["dtype"])
print("labels 数量:", len(LABELS))

def infer(img_pil):
    # 模型输入尺寸
    ih, iw = in_d["shape"][1], in_d["shape"][2]
    im = img_pil.convert("RGB").resize((iw, ih))
    if in_d["dtype"] == np.float32:
        arr = np.array(im, dtype=np.float32) / 255.0   # float32 输入：[0,1]
    else:
        arr = np.array(im, dtype=np.uint8)             # 量化输入：0-255
    interp.set_tensor(in_d["index"], arr[None, ...])
    interp.invoke()
    out = interp.get_tensor(out_d["index"])[0]
    return out

print("\n== 数据集图片推理（按 labels 顺序输出 Top3）==")
for name, fn in TEST:
    p = os.path.join(DS, fn)
    if not os.path.exists(p):
        print(name, "缺图"); continue
    out = infer(Image.open(p))
    idx = np.argsort(out)[::-1]
    top = "  ".join("%s=%.1f%%" % (LABELS[i], out[i] * 100) for i in idx[:3])
    print("%-6s -> %s" % (name, top))
