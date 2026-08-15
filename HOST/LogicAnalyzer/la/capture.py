"""采集会话：控制 MCU 采样 → 下载数据 → 返回 TraceData"""
from __future__ import annotations

import threading
from typing import Callable, Optional

import numpy as np

from la.transport import ShellPort, DumpPort
from model.trace import TraceData


class CaptureError(Exception):
    pass


class CaptureSession:
    def __init__(self, ctrl_port: str, data_port: str,
                 ctrl_baud: int = 115200, data_baud: int = 921600):
        self.ctrl = ShellPort(ctrl_port, ctrl_baud)
        self.data = DumpPort(data_port, data_baud)
        self._lock = threading.Lock()

    def close(self):
        self.ctrl.close()
        self.data.close()

    def configure(self, rate: int, buffer_src: str = "sram",
                  trig_args: Optional[str] = None) -> str:
        # 注：固件已删除 IRAM 缓冲（统一外部 SRAM 32KB 环形），
        # 不再下发 la_dma_buf 切换命令（固件命令表无此命令）。
        with self._lock:
            log = self.ctrl.la_start_dma(rate)
            if trig_args:
                log += self.ctrl.la_set_trigger(trig_args)
        return log

    def capture(self, rate: int, duration_s: float,
                buffer_src: str = "sram", count: Optional[int] = None,
                progress: Optional[Callable[[int, int], None]] = None,
                trig_args: Optional[str] = None,
                nchannels: int = 4) -> TraceData:
        """开始采样 → 等待 duration_s → 停止 → 下载最新缓冲"""
        with self._lock:
            self.ctrl.la_start_dma(rate)
            if trig_args:
                self.ctrl.la_set_trigger(trig_args)

            import time
            time.sleep(max(duration_s, 0.05))
            stop_log = self.ctrl.la_stop_dma()
            if "samples" not in stop_log:
                raise CaptureError(f"la_dma_stop 异常: {stop_log}")

            # 固件统一外部 SRAM 32KB 环形缓冲（无 iram 模式）
            buf_size = 32768
            total = count or buf_size
            if total > buf_size:
                total = buf_size
            samples = self.data.dump(0, total, progress)
            if samples is None:
                raise CaptureError("采样下载超时/不完整")
            return TraceData(samples=np.asarray(samples, dtype=np.uint32),
                             rate=rate, nchannels=nchannels)
