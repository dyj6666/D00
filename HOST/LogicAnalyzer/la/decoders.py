"""协议解码器：UART / I2C / SPI（纯逻辑，可单元测试）

输入均为 numpy 数组：每元素为 32 位采样（bit0..bit7 = 通道0..7）。
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional

import numpy as np


def channel_bits(samples: np.ndarray, ch: int) -> np.ndarray:
    return ((samples >> ch) & 1).astype(np.uint8)


@dataclass
class Packet:
    kind: str                 # START / ADDR / DATA / STOP / FRAME ...
    start: int                # 起始样本下标
    end: int                  # 结束样本下标
    data: bytes = b""
    info: str = ""


# ============================ UART ============================
@dataclass
class UartConfig:
    tx_ch: Optional[int] = 0      # None = 不解码该线
    rx_ch: Optional[int] = 1
    baud: int = 115200
    data_bits: int = 8
    parity: str = "none"          # none / even / odd
    stop_bits: float = 1.0


def _decode_uart_line(bits: np.ndarray, rate: int, cfg: UartConfig,
                      line_name: str) -> List[Packet]:
    spb = rate / cfg.baud         # 每 bit 的样本数（浮点）
    packets: List[Packet] = []
    n = len(bits)
    i = 0
    while i < n - int(spb):
        # 找起始位：下降沿（1→0）后电平保持低至少 0.5 bit
        if bits[i] == 1 and bits[i + 1] == 0:
            start = i
            stable = 0
            j = i + 1
            while j < n and j < i + int(spb * 0.9) and bits[j] == 0:
                stable += 1
                j += 1
            if stable < int(spb * 0.5):
                i += 1
                continue
            center = start + int(spb * 1.5)
            value = 0
            ok = True
            for b in range(cfg.data_bits):
                idx = center + int(b * spb)
                if idx >= n:
                    ok = False
                    break
                value |= (int(bits[idx]) << b)
            if not ok:
                break
            stop_idx = center + int(cfg.data_bits * spb)
            if stop_idx >= n or bits[stop_idx] != 1:
                i = start + 1
                continue
            if cfg.parity != "none":
                p_idx = center + int(cfg.data_bits * spb)
                if p_idx >= n:
                    break
                ones = bin(value).count("1")
                want = (0 if cfg.parity == "even" else 1) if ones % 2 == 0 \
                    else (1 if cfg.parity == "even" else 0)
                if bits[p_idx] != want:
                    i = start + 1
                    continue
            end = min(n, stop_idx + int(spb))
            byte = bytes([value & 0xFF])
            packets.append(Packet(
                kind="FRAME", start=start, end=end, data=byte,
                info=f"{line_name} 0x{value:02X} "
                     f"'{chr(value) if 32 <= value < 127 else '.'}'"))
            i = end
        else:
            i += 1
    return packets


def decode_uart(samples: np.ndarray, rate: int, cfg: UartConfig) -> List[Packet]:
    out: List[Packet] = []
    if cfg.tx_ch is not None:
        out += _decode_uart_line(channel_bits(samples, cfg.tx_ch), rate, cfg, "TX")
    if cfg.rx_ch is not None and cfg.rx_ch != cfg.tx_ch:
        out += _decode_uart_line(channel_bits(samples, cfg.rx_ch), rate, cfg, "RX")
    out.sort(key=lambda p: p.start)
    return out


# ============================ I2C ============================
@dataclass
class I2cConfig:
    scl_ch: int = 0
    sda_ch: int = 1


def decode_i2c(samples: np.ndarray, rate: int, cfg: I2cConfig) -> List[Packet]:
    scl = channel_bits(samples, cfg.scl_ch)
    sda = channel_bits(samples, cfg.sda_ch)
    packets: List[Packet] = []
    n = len(scl)
    i = 1
    in_transaction = False
    first_byte = False          # START 后的第一个字节为地址
    while i < n - 1:
        # START: SCL 高时 SDA 下降
        if (scl[i] == 1 and scl[i - 1] == 1
                and sda[i] == 0 and sda[i - 1] == 1):
            packets.append(Packet("START", i - 1, i))
            in_transaction = True
            first_byte = True
            i += 1
            continue
        # STOP: SCL 高时 SDA 上升
        if (scl[i] == 1 and scl[i - 1] == 1
                and sda[i] == 1 and sda[i - 1] == 0):
            packets.append(Packet("STOP", i - 1, i))
            in_transaction = False
            first_byte = False
            i += 1
            continue
        # 事务中：SCL 上升沿开始采集一个字节（8 数据位 + ACK）
        if in_transaction and scl[i] == 1 and scl[i - 1] == 0:
            byte_start = i
            bits: List[int] = []
            collected = False
            stopped = False
            while len(bits) < 9 and i < n - 1:
                if scl[i] == 1 and scl[i - 1] == 0:
                    bits.append(int(sda[i]))
                    if len(bits) >= 9:
                        i += 1
                        collected = True
                        break
                    # 推进到时钟低，期间逐样本检测 STOP
                    i += 1
                    while i < n - 1 and scl[i] == 1:
                        if (scl[i - 1] == 1 and sda[i] == 1
                                and sda[i - 1] == 0):
                            stopped = True
                            break
                        i += 1
                    if stopped:
                        break
                else:
                    i += 1
            if stopped:
                packets.append(Packet("STOP", i - 1, i))
                in_transaction = False
                first_byte = False
                i += 1
            elif collected:
                byte_val = 0
                for b in bits[:8]:
                    byte_val = (byte_val << 1) | b
                ack = bits[8]
                if first_byte:
                    kind = "ADDR"
                    info = (f"ADDR 0x{byte_val >> 1:02X} "
                            f"{'W' if byte_val & 1 == 0 else 'R'} "
                            f"ACK={'Y' if ack == 0 else 'N'}")
                else:
                    kind = "DATA"
                    info = (f"DATA 0x{byte_val:02X} "
                            f"'{chr(byte_val) if 32 <= byte_val < 127 else '.'}' "
                            f"ACK={'Y' if ack == 0 else 'N'}")
                packets.append(Packet(kind, byte_start, i,
                                      bytes([byte_val]), info))
                first_byte = False
            continue
        i += 1
    return packets


# ============================ SPI ============================
@dataclass
class SpiConfig:
    clk_ch: int = 0
    mosi_ch: Optional[int] = 1
    miso_ch: Optional[int] = 2
    cs_ch: Optional[int] = 3
    cs_active: int = 0            # 0=低有效, 1=高有效
    cpol: int = 0
    cpha: int = 0
    lsb_first: bool = False


def decode_spi(samples: np.ndarray, rate: int, cfg: SpiConfig) -> List[Packet]:
    clk = channel_bits(samples, cfg.clk_ch)
    mosi = channel_bits(samples, cfg.mosi_ch) if cfg.mosi_ch is not None else None
    miso = channel_bits(samples, cfg.miso_ch) if cfg.miso_ch is not None else None
    cs = channel_bits(samples, cfg.cs_ch) if cfg.cs_ch is not None else None

    packets: List[Packet] = []
    n = len(clk)
    i = 0
    while i < n - 1:
        active = cs[i] == cfg.cs_active if cs is not None else True
        if not active:
            i += 1
            continue
        frame_start = i
        mosi_bits: List[int] = []
        miso_bits: List[int] = []
        idle = cfg.cpol
        for _ in range(8):
            if cfg.cpha == 0:
                edge = 1 - idle
                while i < n - 1 and clk[i] != edge:
                    i += 1
                if i >= n - 1:
                    break
                if mosi is not None:
                    mosi_bits.append(int(mosi[i]))
                if miso is not None:
                    miso_bits.append(int(miso[i]))
                i += 1
                while i < n - 1 and clk[i] != idle:
                    i += 1
            else:
                edge = idle
                while i < n - 1 and clk[i] != edge:
                    i += 1
                if i >= n - 1:
                    break
                if mosi is not None:
                    mosi_bits.append(int(mosi[i]))
                if miso is not None:
                    miso_bits.append(int(miso[i]))
                i += 1
                while i < n - 1 and clk[i] != (1 - idle):
                    i += 1
            i += 1
        if len(mosi_bits) == 8 or len(miso_bits) == 8:
            def pack(bits):
                v = 0
                seq = bits if not cfg.lsb_first else bits[::-1]
                for b in seq:
                    v = (v << 1) | b
                return v
            parts = []
            data = b""
            if len(mosi_bits) == 8:
                mv = pack(mosi_bits)
                parts.append(f"MOSI 0x{mv:02X}")
                data += bytes([mv])
            if len(miso_bits) == 8:
                sv = pack(miso_bits)
                parts.append(f"MISO 0x{sv:02X}")
                data += bytes([sv])
            packets.append(Packet("SPI_FRAME", frame_start, i, data,
                                  " / ".join(parts)))
        while i < n - 1 and (cs is None or cs[i] == cfg.cs_active):
            i += 1
        i += 1
    return packets
