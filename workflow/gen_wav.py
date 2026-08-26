# -*- coding: utf-8 -*-
"""生成内置测试 WAV（小星星前两句）并转 C 数组（wav_data.c/h）。

旋律：C C G G A A G | F F E E D D C（C 大调，16kHz/16bit/单声道）
输出：APP/APP/SystemServices/wav_data.c / wav_data.h
"""
import math
import struct
import wave
from pathlib import Path

SR = 16000
OUT_C = Path(r"D:\GIT-SPACE\D00\APP\APP\SystemServices\wav_data.c")
OUT_H = Path(r"D:\GIT-SPACE\D00\APP\APP\SystemServices\wav_data.h")

# 音符频率（Hz）与时长（秒）；0 = 休止
NOTES = [
    (523.25, 0.30), (523.25, 0.30), (783.99, 0.30), (783.99, 0.30),
    (880.00, 0.30), (880.00, 0.30), (783.99, 0.60),
    (698.46, 0.30), (698.46, 0.30), (659.25, 0.30), (659.25, 0.30),
    (587.33, 0.30), (587.33, 0.30), (523.25, 0.60),
]

def gen_wav() -> bytes:
    buf = bytearray()
    for freq, dur in NOTES:
        n = int(SR * dur)
        for i in range(n):
            # 简单包络：前 10ms 淡入、后 20ms 淡出，避免爆音
            env = 1.0
            fade_in = int(SR * 0.01)
            fade_out = int(SR * 0.02)
            if i < fade_in:
                env = i / fade_in
            elif i > n - fade_out:
                env = max(0.0, (n - i) / fade_out)
            v = int(14000 * env * math.sin(2.0 * math.pi * freq * i / SR))
            buf += struct.pack("<h", v)
        # 音间 30ms 静音
        for _ in range(int(SR * 0.03)):
            buf += struct.pack("<h", 0)
    return bytes(buf)

def build_wav(pcm: bytes) -> bytes:
    with wave.open(io := __import__("io").BytesIO(), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm)
        return io.getvalue()

pcm = gen_wav()
wav = build_wav(pcm)
print(f"WAV: {len(wav)} bytes, PCM {len(pcm)} bytes, {len(pcm)/2/SR:.2f}s @ {SR}Hz")

# 转 C 数组（16 字节一行）
rows = []
for i in range(0, len(wav), 16):
    chunk = wav[i:i + 16]
    rows.append("    " + "".join(f"0x{b:02X}," for b in chunk))
body = "\n".join(rows)

OUT_C.write_text(
    "/* ================================================================\n"
    " * wav_data.c —— 内置测试音频（小星星前两句，16kHz/16bit/单声道）\n"
    " *\n"
    " * 生成：workflow 脚本 gen_wav.py（勿手改）\n"
    " * ================================================================ */\n"
    '#include "wav_data.h"\n\n'
    f"const uint8_t g_wav_star[] = {{\n{body}\n}};\n"
    f"const uint32_t g_wav_star_len = {len(wav)}u;\n",
    encoding="utf-8", newline="\n")

OUT_H.write_text(
    "/* ================================================================\n"
    " * wav_data.h —— 内置测试音频声明\n"
    " * ================================================================ */\n"
    "#ifndef WAV_DATA_H\n"
    "#define WAV_DATA_H\n\n"
    "#include <stdint.h>\n\n"
    "extern const uint8_t g_wav_star[];\n"
    "extern const uint32_t g_wav_star_len;\n\n"
    "#endif /* WAV_DATA_H */\n",
    encoding="utf-8", newline="\n")

print(f"written: {OUT_C} / {OUT_H}")
