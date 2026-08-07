#!/usr/bin/env python3
"""CPU 极限负载测试：全外设满负载下采集 sysmon CPU/栈水位数据。

压力源：
  - LA 6MHz DMA 采样（TIM1 触发）
  - USART6 921600 信号发生器输出（阻塞版）
  - SPI2 信号发生器输出（DMA 版）
  - HOSTLINK 全量 LA_DUMP 32768 样本持续传输
"""

import sys
import time
import struct
import threading

import serial

sys.path.insert(0, r"D:\GIT-SPACE\D00\HOST\OTA_Tool")
from core.hostlink import build_frame, FrameParser  # noqa: E402

CTRL = "COM9"
DATA = "COM13"
CTRL_BAUD = 115200
DATA_BAUD = 921600


def shell_cmd(ser, cmd, wait=0.8):
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())
    time.sleep(wait)
    out = b""
    deadline = time.time() + wait + 0.4
    while time.time() < deadline:
        n = ser.in_waiting
        if n:
            out += ser.read(n)
        else:
            time.sleep(0.03)
    return out.decode("utf-8", errors="replace")


def dump_all(ser, count=32768):
    ser.reset_input_buffer()
    ser.write(build_frame(0x07, struct.pack("<II", 0, count)))
    parser = FrameParser()
    samples = 0
    t0 = time.time()
    deadline = t0 + 8 + count / 80000.0
    while samples < count and time.time() < deadline:
        n = ser.in_waiting
        if n:
            parser.feed(ser.read(n))
            for fr in parser.frames():
                if fr[2] != 0x07:
                    continue
                pl = FrameParser.payload(fr)
                if len(pl) < 6:
                    continue
                samples += struct.unpack("<H", pl[4:6])[0]
        else:
            time.sleep(0.002)
    return samples, time.time() - t0


def main() -> int:
    ctrl = serial.Serial(CTRL, CTRL_BAUD, timeout=0.3)
    data = serial.Serial(DATA, DATA_BAUD, timeout=0.3)

    def run_scenario(name, sg_cmd, sg_stop):
        print(f"\n########## SCENARIO: {name} ##########")
        print(shell_cmd(ctrl, "la_dma_start 6000000", 0.5), end="")
        print(shell_cmd(ctrl, sg_cmd, 0.5), end="")

        result = {}

        def sample_sysmon():
            time.sleep(0.4)
            result["sysmon"] = shell_cmd(ctrl, "sysmon", 1.0)

        t = threading.Thread(target=sample_sysmon, daemon=True)
        t.start()
        samples, dt = dump_all(data, 32768)
        t.join(timeout=4)

        print(f"[STRESS] LA_DUMP 32768 -> got={samples} time={dt:.2f}s "
              f"rate={samples * 4 / dt / 1000:.1f}KB/s")
        print("\n===== sysmon DURING STRESS =====")
        print(result.get("sysmon", "(no sysmon capture)"), end="")

        print(shell_cmd(ctrl, sg_stop, 0.3), end="")
        print(shell_cmd(ctrl, "la_dma_stop", 0.5), end="")

    run_scenario("LA 6MHz + SPI2 DMA + HOSTLINK dump",
                 "sg_spi_start 55AA55AA55AA55AA 1", "sg_spi_stop")
    run_scenario("LA 6MHz + USART6 921600 + HOSTLINK dump",
                 "sg_uart_start 921600 55AA55AA55AA55AA 1", "sg_uart_stop")
    run_scenario("LA 6MHz + HOSTLINK dump (baseline)",
                 "sg_spi_stop", "sg_spi_stop")

    print(shell_cmd(ctrl, "sysmon", 1.0), end="")

    ctrl.close()
    data.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
