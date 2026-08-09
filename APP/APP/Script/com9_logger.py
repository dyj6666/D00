"""COM9 日志抓取：可选在打开串口后立即发送一条命令（如 reset）。

用法：
    python com9_logger.py <out> <seconds> [--cmd "reset"] [--port COM9] [--baud 115200]
"""

import argparse
import sys
import time

import serial

parser = argparse.ArgumentParser()
parser.add_argument("out", nargs="?", default=r"D:\GIT-SPACE\D00\APP\APP\_com9_boot.txt")
parser.add_argument("seconds", nargs="?", type=int, default=20)
parser.add_argument("--cmd", default=None, help="打开串口后立即发送的命令（如 reset）")
parser.add_argument("--port", default="COM9")
parser.add_argument("--baud", type=int, default=115200)
args = parser.parse_args()

ser = serial.Serial(args.port, args.baud, timeout=0.2)
if args.cmd:
    time.sleep(0.3)
    ser.reset_input_buffer()
    ser.write((args.cmd + "\r").encode())
end = time.time() + args.seconds
with open(args.out, "w", encoding="utf-8", errors="replace") as f:
    while time.time() < end:
        try:
            n = ser.in_waiting
            d = ser.read(n if n else 1)
            if d:
                f.write(d.decode("utf-8", "replace"))
                f.flush()
        except Exception:
            break
ser.close()
