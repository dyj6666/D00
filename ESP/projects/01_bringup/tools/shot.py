#!/usr/bin/env python
"""shot.py —— 从串口抓取 ESP32 屏幕截图并转成 PNG（视觉回归用）

原理：
    固件里 ui_debug_dump_screen() 用「flush 回调拼帧」的方式合成整屏，
    经控制台串口回传：
        "DUMP <w> <h>\\n" + w*h*2 字节 RGB565(小端) + "\\nDUMP_END\\n"

用法：
    python tools/shot.py --port COM9 --out build/shot.png
    python tools/shot.py --port COM9 --raw build/shot.raw   # 只存原始数据

前置：
    把 components/ui/src/ui_port.c 里的 UI_DEBUG_SHOT 置 1（上电 6s 后自动
    切到 FACE 页并回传一张），或调用 ui_debug_dump_screen()。
"""
import argparse
import os
import sys
import time

import serial


def read_until(ser, marker: bytes, timeout_s: float) -> bytes:
    buf = b""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        chunk = ser.read(65536)
        if chunk:
            buf += chunk
            if marker in buf:
                return buf
    return buf


def grab(port: str, timeout_s: float):
    ser = serial.Serial(port, 115200, timeout=0.2)
    try:
        # 硬复位：RTS 拉低再释放（ESP32 EN 脚）
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.15)
        ser.setRTS(False)

        buf = read_until(ser, b"DUMP ", timeout_s)
        idx = buf.find(b"DUMP ")
        if idx < 0:
            raise RuntimeError("未见 DUMP 头（UI_DEBUG_SHOT 是否置 1？）")
        buf = buf[idx:]

        head, _, rest = buf.partition(b"\n")
        parts = head.split()
        if len(parts) < 3:
            raise RuntimeError(f"DUMP 头格式异常: {head!r}")
        w, h = int(parts[1]), int(parts[2])
        want = w * h * 2

        data = bytearray(rest)
        t0 = time.time()
        while len(data) < want and time.time() - t0 < timeout_s:
            chunk = ser.read(65536)
            if chunk:
                data += chunk
        return w, h, bytes(data[:want])
    finally:
        ser.close()


def raw_to_png(raw: bytes, w: int, h: int, out_png: str, scale: int = 2) -> None:
    from PIL import Image

    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        base = y * w * 2
        for x in range(w):
            v = raw[base + 2 * x] | (raw[base + 2 * x + 1] << 8)
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            px[x, y] = ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
    if scale > 1:
        img = img.resize((w * scale, h * scale), Image.NEAREST)
    img.save(out_png)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM9")
    ap.add_argument("--out", default="build/shot.png")
    ap.add_argument("--raw", default="")
    ap.add_argument("--timeout", type=float, default=45.0)
    args = ap.parse_args()

    w, h, raw = grab(args.port, args.timeout)
    print(f"抓取完成：{w}x{h}，{len(raw)} 字节")

    if args.raw:
        os.makedirs(os.path.dirname(args.raw) or ".", exist_ok=True)
        with open(args.raw, "wb") as f:
            f.write(raw)
        print(f"原始数据 -> {args.raw}")

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    raw_to_png(raw, w, h, args.out)
    print(f"PNG -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
