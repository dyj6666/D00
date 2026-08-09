"""临时工具：COM9 日志抓取（供 SWD 复位后查看启动/崩溃信息）。"""

import sys
import time

import serial

OUT = sys.argv[1] if len(sys.argv) > 1 else r"D:\GIT-SPACE\D00\APP\APP\_com9_boot.txt"
SECONDS = int(sys.argv[2]) if len(sys.argv) > 2 else 20

ser = serial.Serial("COM9", 115200, timeout=0.2)
end = time.time() + SECONDS
with open(OUT, "w", encoding="utf-8", errors="replace") as f:
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
