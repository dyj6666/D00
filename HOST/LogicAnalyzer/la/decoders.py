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


@dataclass
class BitAnno:
    """波形协议位标注：样本区间 [start, end) + 标签 + 语义类别。"""
    start: int
    end: int
    label: str
    kind: str        # start / data / stop / addr / ack / frame / idle
    ch: Optional[int] = None   # 标注归属通道（None=顶部通用层）


def _decode_uart_line(bits: np.ndarray, rate: int, cfg: UartConfig,
                      line_name: str) -> List[Packet]:
    spb = rate / cfg.baud         # 每 bit 的样本数（浮点）
    packets: List[Packet] = []
    if spb < 2.0:
        # 每 bit 不足 2 个样本无法可靠解码（如 100kHz 采样 115200 波特率），
        # 提前返回避免 int(spb)==0 时循环越界。
        return packets
    n = len(bits)
    i = 0
    while i < n - 1:
        # 找起始位：下降沿（1→0）后电平保持低至少 0.5 bit。
        # 注意 start 取下降沿后的第一个低样本（i+1），否则起始位中心整体
        # 前移 1 个样本，数据位采样在非整数 spb 下会错位。
        if bits[i] == 1 and bits[i + 1] == 0:
            start = i + 1
            stable = 0
            j = i + 1
            while j < n and j < i + int(spb * 0.9) and bits[j] == 0:
                stable += 1
                j += 1
            if stable < int(spb * 0.5):
                i += 1
                continue
            # 浮点中心 + 逐位四舍五入：避免非整数 spb 下取整误差随位号累积，
            # 导致高位采样点漂移到 bit 边界（1MHz@115200 时 spb=8.68）。
            center_f = start + 1.5 * spb
            value = 0
            ok = True
            for b in range(cfg.data_bits):
                idx = int(center_f + b * spb + 0.5)
                if idx >= n:
                    ok = False
                    break
                value |= (int(bits[idx]) << b)
            if not ok:
                break
            stop_idx = int(center_f + cfg.data_bits * spb + 0.5)
            if stop_idx >= n or bits[stop_idx] != 1:
                i = start + 1
                continue
            if cfg.parity != "none":
                p_idx = stop_idx
                if p_idx >= n:
                    break
                ones = bin(value).count("1")
                want = (0 if cfg.parity == "even" else 1) if ones % 2 == 0 \
                    else (1 if cfg.parity == "even" else 0)
                if bits[p_idx] != want:
                    i = start + 1
                    continue
            # end 停在停止位中心：若按帧尾（+1 bit）收尾会越过停止位结束，
            # 漏掉连续字节下一帧的起始位下降沿（back-to-back 传输时会把
            # 数据位边沿误判为起始位，导致后续字节全部错位）。
            end = min(n, int(start + (1 + cfg.data_bits + cfg.stop_bits * 0.5) * spb))
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


def annotate_uart(samples: np.ndarray, rate: int, cfg: UartConfig) -> List[BitAnno]:
    """按波特率把 UART 帧逐位标注：起始位 S / 数据位 Dn=v / 停止位 STOP。
    返回样本区间与标签列表，供波形层绘制彩色协议位色块。"""
    spb = rate / cfg.baud
    if spb < 2.0:
        return []
    bits = channel_bits(samples, cfg.tx_ch)
    n = len(bits)
    out: List[BitAnno] = []
    i = 0
    while i < n - 1:
        if bits[i] == 1 and bits[i + 1] == 0:
            start = i + 1
            stable = 0
            j = start
            while j < n and j < i + int(spb * 0.9) and bits[j] == 0:
                stable += 1
                j += 1
            if stable < int(spb * 0.5):
                i += 1
                continue
            out.append(BitAnno(start, int(start + spb), "S", "start",
                               ch=cfg.tx_ch))
            center = start + 1.5 * spb
            value = 0
            for b in range(cfg.data_bits):
                v = int(bits[int(center + b * spb + 0.5)])
                value |= v << b
                bs = int(start + (1 + b) * spb)
                be = int(start + (2 + b) * spb)
                out.append(BitAnno(bs, be, f"D{b}={v}", "data",
                                   ch=cfg.tx_ch))
            stop_start = int(start + (1 + cfg.data_bits) * spb)
            out.append(BitAnno(stop_start,
                               int(stop_start + cfg.stop_bits * spb),
                               "STOP", "stop", ch=cfg.tx_ch))
            frame_end = int(start + (1 + cfg.data_bits + cfg.stop_bits) * spb)
            ch = chr(value) if 32 <= value < 127 else "."
            out.append(BitAnno(start, frame_end,
                               f"0x{value:02X} '{ch}'", "byte"))
            # 推进到停止位中心：若推到帧尾会漏掉连续字节的下一帧起始位下降沿
            i = int(start + (1 + cfg.data_bits + cfg.stop_bits * 0.5) * spb)
        else:
            i += 1
    return out


def annotate_i2c(samples: np.ndarray, rate: int, cfg: I2cConfig) -> List[BitAnno]:
    """I2C 帧级标注：复用解码结果，把 START/ADDR/DATA/ACK/STOP 映射为色块。"""
    pkts = decode_i2c(samples, rate, cfg)
    return [BitAnno(p.start, p.end, p.kind, p.kind.lower()) for p in pkts]


def annotate_spi(samples: np.ndarray, rate: int, cfg: SpiConfig) -> List[BitAnno]:
    """SPI 帧级标注：每帧一个色块，标签带 MOSI/MISO 值摘要。"""
    pkts = decode_spi(samples, rate, cfg)
    out = []
    for p in pkts:
        label = p.info[:24] if p.info else "FRAME"
        out.append(BitAnno(p.start, p.end, label, "frame"))
    return out


def annotate_spi_bits(samples: np.ndarray, rate: int,
                      cfg: SpiConfig) -> List[BitAnno]:
    """SPI 位级标注：CS 有效沿标 CS，每字节 8 个 bit（MSB 优先 b7..b0），
    字节级标十六进制+二进制（如 0xA5 10100101）。"""
    clk = channel_bits(samples, cfg.clk_ch)
    mosi = channel_bits(samples, cfg.mosi_ch) if cfg.mosi_ch is not None else None
    miso = channel_bits(samples, cfg.miso_ch) if cfg.miso_ch is not None else None
    cs = channel_bits(samples, cfg.cs_ch) if cfg.cs_ch is not None else None
    n = len(clk)
    out: List[BitAnno] = []
    i = 0
    idle = cfg.cpol
    prev_active = False
    while i < n - 1:
        active = cs[i] == cfg.cs_active if cs is not None else True
        if active and not prev_active:
            out.append(BitAnno(i, i + 1, "CS\u2193", "cs", ch=cfg.cs_ch))
        elif not active and prev_active:
            out.append(BitAnno(i, i + 1, "CS\u2191", "cs", ch=cfg.cs_ch))
        if active:
            j = i
            bits_pos = []
            ok = True
            for _ in range(8):
                edge = 1 - idle
                while j < n - 1 and clk[j] != edge:
                    j += 1
                if j >= n - 1 or (cs is not None and cs[j] != cfg.cs_active):
                    ok = False
                    break
                bs = j
                v = int(mosi[j]) if mosi is not None else 0
                j += 1
                while j < n - 1 and clk[j] != idle:
                    j += 1
                be = j
                j += 1
                bits_pos.append((bs, be, v))
            if ok and len(bits_pos) == 8:
                value = 0
                for k, (bs, be, v) in enumerate(bits_pos):
                    bit_no = 7 - k          # MSB first：先采到的是 b7
                    out.append(BitAnno(bs, be, f"b{bit_no}={v}", "data",
                                       ch=cfg.mosi_ch))
                    value = (value << 1) | v
                out.append(BitAnno(bits_pos[0][0], bits_pos[-1][1],
                                   f"0x{value:02X} {value:08b}", "byte"))
                if miso is not None:
                    for k, (bs, be, _) in enumerate(bits_pos):
                        mv = int(miso[bs])
                        out.append(BitAnno(bs, be, f"b{7-k}={mv}", "miso",
                                           ch=cfg.miso_ch))
                i = j
                prev_active = active
                continue
        prev_active = active
        i += 1
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
        # 解完一字节后 i 已停在帧内 CLK 空闲段，由外层循环按 cs 有效性
        # 继续解下一字节；不得把 i 推进到 CS 拉高（否则多字节帧只解第一字节）
    return packets
