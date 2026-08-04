"""采样数据模型：通道提取、时间基准、位/十六进制/十进制视图"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

import numpy as np


@dataclass
class TraceData:
    samples: np.ndarray        # uint32, bit0..7 = 通道 0..7
    rate: int                  # 采样率 Hz
    nchannels: int = 8

    @property
    def count(self) -> int:
        return int(self.samples.size)

    @property
    def duration_us(self) -> float:
        return self.count / self.rate * 1e6

    def channel(self, ch: int) -> np.ndarray:
        return ((self.samples >> ch) & 1).astype(np.uint8)

    def time_axis_us(self) -> np.ndarray:
        return np.arange(self.count, dtype=np.float64) / self.rate * 1e6

    def bit_string(self, ch: int, start: int, end: int) -> str:
        return "".join(map(str, self.channel(ch)[start:end]))

    def extract_bytes(self, ch: int, start: int, end: int,
                      bit_order: str = "lsb") -> Optional[int]:
        bits = self.channel(ch)[start:end]
        if bits.size == 0:
            return None
        seq = bits if bit_order == "lsb" else bits[::-1]
        v = 0
        for b in seq:
            v = (v << 1) | int(b)
        return v
