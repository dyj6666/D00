# -*- coding: utf-8 -*-
"""生成内置 WAV（小星星演示 + 天空之城开机旋律）并转 C 数组。

输出：APP/APP/SystemServices/wav_data.c / wav_data.h
  1) g_wav_star     小星星前两句（16kHz/16bit，Audio 页演示）
  2) g_wav_startup  天空之城主题前奏 + 和弦伴奏（16kHz，开机旋律）

合成：正弦 + 2 次谐波(30%) + 钢琴式包络(快起音/指数衰减)，和弦垫底。
"""
import math
import struct
import wave
from pathlib import Path

SR = 16000
OUT_C = Path(r"D:\GIT-SPACE\D00\APP\APP\SystemServices\wav_data.c")
OUT_H = Path(r"D:\GIT-SPACE\D00\APP\APP\SystemServices\wav_data.h")

# ---------------- 音符工具 ----------------
NOTE = {
    "C3": 130.81, "D3": 146.83, "E3": 164.81, "F3": 174.61, "G3": 196.00,
    "A3": 220.00, "B3": 246.94,
    "C4": 261.63, "D4": 293.66, "E4": 329.63, "F4": 349.23, "G4": 392.00,
    "A4": 440.00, "B4": 493.88,
    "C5": 523.25, "D5": 587.33, "E5": 659.25, "F5": 698.46, "G5": 783.99,
}

def tone(freq, dur, amp=0.5, decay=6.0, harmonics=((1.0, 1.0), (2.0, 0.3))):
    """单音：正弦 + 谐波 + 指数衰减包络（钢琴感）"""
    n = int(SR * dur)
    out = []
    for i in range(n):
        t = i / SR
        env = math.exp(-decay * t)          # 指数衰减
        env *= min(1.0, i / max(1, int(SR * 0.004)))   # 4ms 快起音
        v = 0.0
        for mult, amp_h in harmonics:
            v += amp_h * math.sin(2.0 * math.pi * freq * mult * t)
        out.append(int(32767 * amp * env * v))
    return out

def silence(dur):
    return [0] * int(SR * dur)

def mix(*tracks):
    n = max(len(t) for t in tracks)
    out = []
    for i in range(n):
        v = 0.0
        for t in tracks:
            if i < len(t):
                v += t[i] / 32767.0
        out.append(max(-32767, min(32767, int(v * 32767 * 0.82))))  # 防削波
    return out

def build_wav(pcm):
    """PCM → 标准 WAV（RIFF/WAVE/fmt/data，16bit 单声道）"""
    import io
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))
    return buf.getvalue()

def to_c(name, wav):
    rows = []
    for i in range(0, len(wav), 16):
        chunk = wav[i:i + 16]
        rows.append("    " + "".join(f"0x{b:02X}," for b in chunk))
    return (f"const uint8_t {name}[] = {{\n" + "\n".join(rows) +
            f"\n}};\nconst uint32_t {name}_len = {len(wav)}u;\n")

# ---------------- 1) 小星星（保留） ----------------
def gen_star():
    freqs = [523.25, 523.25, 783.99, 783.99, 880.0, 880.0, 783.99,
             698.46, 698.46, 659.25, 659.25, 587.33, 587.33, 523.25]
    durs = [0.30] * 6 + [0.60, 0.30, 0.30, 0.30, 0.30, 0.30, 0.30, 0.60]
    pcm = []
    for f, d in zip(freqs, durs):
        pcm += tone(f, d, amp=0.5)
        pcm += silence(0.03)
    return build_wav(pcm)

# ---------------- 2) 天空之城主题前奏（开机旋律） ----------------
def gen_startup():
    # 主旋律（《伴随着你》主题开头，C 大调）：A4 B4 C5 B4 C5 E5 B4 E5
    melody = [("A4", 0.45), ("B4", 0.45), ("C5", 0.45), ("B4", 0.40),
              ("C5", 0.40), ("E5", 0.60), ("B4", 0.35), ("E5", 0.90)]
    # 和弦伴奏（每 2 音一个和弦长音）：Am F C G Am F C E
    chords = [["A3", "C4", "E4"], ["F3", "A3", "C4"], ["C4", "E4", "G4"],
              ["G3", "B3", "D4"], ["A3", "C4", "E4"], ["F3", "A3", "C4"],
              ["C4", "E4", "G4"], ["E3", "G3", "B3"]]

    mel = []
    acc = []
    t_mel = 0.0
    for i, (name, d) in enumerate(melody):
        mel += tone(NOTE[name], d, amp=0.55, decay=5.0)
        # 和弦：覆盖该音及其后 0.15s（与下一个和弦衔接）
        chord_dur = d + (0.15 if i + 1 < len(melody) else 0.5)
        chord = [0] * int(SR * chord_dur)
        for cname in chords[i % len(chords)]:
            t = tone(NOTE[cname], chord_dur, amp=0.30, decay=3.0)
            for k, v in enumerate(t):
                chord[k] += v
        # 补齐 mel 与 acc 长度差（当前音长度 + 0.15 衔接）
        need = len(mel) - len(acc)
        if need > 0:
            acc += chord[:need]
        else:
            acc = acc[:len(mel)]
        # 音间 40ms 气口
        mel += silence(0.04)
        acc += silence(0.04)

    return build_wav(mix(mel, acc))

# ---------------- 输出 ----------------
star = gen_star()
startup = gen_startup()
print(f"star   : {len(star)} B ({len(star)/2/SR:.2f}s)")
print(f"startup: {len(startup)} B ({len(startup)/2/SR:.2f}s)")

body = (to_c("g_wav_star", star) + "\n" + to_c("g_wav_startup", startup))
OUT_C.write_text(
    "/* ================================================================\n"
    " * wav_data.c —— 内置音频数据\n"
    " *   g_wav_star     小星星前两句（16kHz/16bit，Audio 页演示）\n"
    " *   g_wav_startup  天空之城主题前奏 + 和弦（开机旋律）\n"
    " *\n"
    " * 生成：workflow/gen_wav.py（勿手改）\n"
    " * ================================================================ */\n"
    '#include "wav_data.h"\n\n' + body,
    encoding="utf-8", newline="\n")

OUT_H.write_text(
    "/* ================================================================\n"
    " * wav_data.h —— 内置音频数据声明\n"
    " * ================================================================ */\n"
    "#ifndef WAV_DATA_H\n"
    "#define WAV_DATA_H\n\n"
    "#include <stdint.h>\n\n"
    "extern const uint8_t g_wav_star[];\n"
    "extern const uint32_t g_wav_star_len;\n"
    "extern const uint8_t g_wav_startup[];\n"
    "extern const uint32_t g_wav_startup_len;\n\n"
    "#endif /* WAV_DATA_H */\n",
    encoding="utf-8", newline="\n")

print(f"written: {OUT_C} / {OUT_H}")
