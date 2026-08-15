"""调试串口日志抓取：可选在打开串口后立即发送一条命令（如 reset）。

用法：
    python com9_logger.py <out> <seconds> [--cmd "reset"] [--port COM5] [--baud 115200]
端口默认自动探测（优先 CH340 即探索者板载调试串口；换 USB 口导致 COM 号漂移时无需改参）。
"""

import argparse
import sys
import threading
import time

import serial
import serial.tools.list_ports


def auto_debug_port():
    """自动定位调试串口：优先 CH340/CH9102 描述，其次 COM5，最后任意可用串口。"""
    fallback = "COM5"
    try:
        comports = serial.tools.list_ports.comports()
        if comports:
            for p in comports:
                desc = (p.description or "").upper()
                if "CH340" in desc or "CH9102" in desc:
                    return p.device
            return comports[0].device
    except Exception:
        pass
    return fallback

parser = argparse.ArgumentParser()
parser.add_argument("out", nargs="?", default=r"D:\GIT-SPACE\D00\APP\APP\_com9_boot.txt")
parser.add_argument("seconds", nargs="?", type=int, default=20)
parser.add_argument("--cmd", default=None, help="打开串口后立即发送的命令（如 reset）")
parser.add_argument("--cmd-delay", type=float, default=0.0,
                    help="命令延迟发送秒数（如复位后 5s 待板子启动再发 lcd selftest）")
parser.add_argument("--port", default=None, help="调试串口（默认自动探测）")
parser.add_argument("--baud", type=int, default=115200)
args = parser.parse_args()

port = args.port or auto_debug_port()
ser = serial.Serial(port, args.baud, timeout=0.2)


def delayed_cmd():
    """延时发送命令（与日志读取并行：复位后板子启动日志不被吞）"""
    time.sleep(max(0.3, args.cmd_delay))
    try:
        ser.reset_input_buffer()
        ser.write((args.cmd + "\r").encode())
    except Exception:
        pass


if args.cmd:
    threading.Thread(target=delayed_cmd, daemon=True).start()
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
