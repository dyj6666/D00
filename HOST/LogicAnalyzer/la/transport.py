"""串口传输封装：控制口（shell, 115200）与数据口（HOSTLINK, 921600）"""
from __future__ import annotations

import time
from typing import Optional

import serial


class ShellPort:
    """调试 shell 控制口（la_dma_start/stop/buf/trig 等）"""

    def __init__(self, port: str, baud: int = 115200):
        self.ser = serial.Serial(port, baud, timeout=0.4)

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def command(self, cmd: str, wait_s: float = 0.6) -> str:
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\r\n").encode())
        time.sleep(wait_s)
        out = b""
        deadline = time.time() + wait_s + 0.5
        while time.time() < deadline:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                out += chunk
            else:
                time.sleep(0.05)
        return out.decode(errors="replace")

    def la_start_dma(self, rate: int) -> str:
        return self.command(f"la_dma_start {rate}")

    def la_stop_dma(self) -> str:
        return self.command("la_dma_stop", wait_s=1.0)

    def la_set_buffer(self, src: str) -> str:
        return self.command(f"la_dma_buf {src}")

    def la_set_trigger(self, trig_args: str) -> str:
        return self.command(f"la_trig {trig_args}")


class DumpPort:
    """HOSTLINK 数据口：二进制采样下载"""

    def __init__(self, port: str, baud: int = 921600):
        self.ser = serial.Serial(port, baud, timeout=1.0)

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def dump(self, offset: int, count: int,
             progress=None) -> Optional[list]:
        """下载 DMA 缓冲原始采样；progress(count, total) 回调"""
        from la.protocol import build_la_dump_request, FrameParser

        self.ser.reset_input_buffer()
        self.ser.write(build_la_dump_request(offset, count))
        parser = FrameParser()
        samples: list = []
        deadline = time.time() + 5 + count / 200000.0
        while len(samples) < count and time.time() < deadline:
            data = self.ser.read(1024)
            if not data:
                time.sleep(0.01)
                continue
            parser.feed(data)
            for frame in parser.frames():
                if len(frame) < 11 or frame[2] != 0x07:
                    continue
                plen = int.from_bytes(frame[3:5], "little")
                n = int.from_bytes(frame[9:11], "little")
                for i in range(n):
                    samples.append(int.from_bytes(frame[11 + i * 4:15 + i * 4],
                                                  "little"))
            if progress:
                progress(min(len(samples), count), count)
        if len(samples) < count:
            return None
        return samples[:count]
