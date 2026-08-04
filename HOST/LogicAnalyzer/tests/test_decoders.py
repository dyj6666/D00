"""解码器与协议单元测试（合成波形）"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import numpy as np

from la import decoders as dec
from la.protocol import build_frame, FrameParser, crc16

failures = 0


def check(cond, msg):
    global failures
    if cond:
        print(f"  ok: {msg}")
    else:
        print(f"  FAIL: {msg}")
        failures += 1


def synth_samples(n=200000, rate=1000000):
    return np.zeros(n, dtype=np.uint32)


def set_bit(samples, ch, idx, val):
    mask = 0xFFFFFFFF ^ (1 << ch)
    samples[idx] = (samples[idx] & mask) | (val << ch)


def test_uart():
    rate = 1000000
    baud = 100000          # 10 样本/bit
    spb = rate // baud
    samples = synth_samples(3000, rate)
    # 在 CH0 发送字节 0x55 (01010101, LSB first: 1 0 1 0 1 0 1 0)
    data = [1, 0, 1, 0, 1, 0, 1, 0]
    set_bit(samples, 0, 0, 1)
    for bit_i, b in enumerate(data):
        for k in range(spb):
            set_bit(samples, 0, spb + bit_i * spb + k, b)
    # 停止位（空闲高）
    for k in range(spb * 2):
        set_bit(samples, 0, spb * 9 + k, 1)
    cfg = dec.UartConfig(tx_ch=0, rx_ch=None, baud=baud)
    pkts = dec.decode_uart(samples, rate, cfg)
    check(len(pkts) == 1, "UART 解出 1 帧")
    if pkts:
        check(pkts[0].data[0] == 0x55, f"UART 数据 0x55（实际 0x{pkts[0].data[0]:02X}）")


def test_i2c():
    rate = 1000000
    samples = synth_samples(4000, rate)
    scl, sda = 0, 1
    # START
    set_bit(samples, scl, 0, 1); set_bit(samples, sda, 0, 1)
    for k in range(1, 10):
        set_bit(samples, scl, k, 1); set_bit(samples, sda, k, 1)
    # SDA 下降（SCL 高）→ START
    for k in range(10, 20):
        set_bit(samples, scl, k, 1); set_bit(samples, sda, k, 0)
    # 地址 0x50<<1 | 0 = 0xA0, MSB first: 1 0 1 0 0 0 0 0, ACK=0
    addr_bits = [1, 0, 1, 0, 0, 0, 0, 0]
    idx = 20
    for b in addr_bits:
        for k in range(10):
            set_bit(samples, sda, idx + k, b)
        set_bit(samples, scl, idx + 4, 1)   # 上升沿采样
        set_bit(samples, scl, idx + 8, 0)
        idx += 10
    for k in range(10):
        set_bit(samples, sda, idx + k, 0)    # ACK
    set_bit(samples, scl, idx + 4, 1)
    set_bit(samples, scl, idx + 8, 0)
    idx += 20
    # 数据 0x2A: 0 0 1 0 1 0 1 0, ACK=0
    data_bits = [0, 0, 1, 0, 1, 0, 1, 0]
    for b in data_bits:
        for k in range(10):
            set_bit(samples, sda, idx + k, b)
        set_bit(samples, scl, idx + 4, 1)
        set_bit(samples, scl, idx + 8, 0)
        idx += 10
    for k in range(10):
        set_bit(samples, sda, idx + k, 0)
    set_bit(samples, scl, idx + 4, 1)
    set_bit(samples, scl, idx + 8, 0)
    idx += 20
    # STOP: SCL 高时 SDA 上升
    for k in range(10):
        set_bit(samples, scl, idx + k, 1)
        set_bit(samples, sda, idx + k, 0)
    for k in range(10, 20):
        set_bit(samples, scl, idx + k, 1)
        set_bit(samples, sda, idx + k, 1)

    cfg = dec.I2cConfig(scl_ch=scl, sda_ch=sda)
    pkts = dec.decode_i2c(samples, rate, cfg)
    kinds = [p.kind for p in pkts]
    check(kinds.count("ADDR") == 1, f"I2C 地址帧（kinds={kinds}）")
    check(kinds.count("DATA") == 1, "I2C 数据帧")
    if pkts:
        check(pkts[0].kind == "START", "I2C START")
        check(pkts[-1].kind == "STOP", "I2C STOP")
        for p in pkts:
            if p.kind == "ADDR":
                check(p.data[0] == 0xA0, f"I2C 地址 0xA0（实际 0x{p.data[0]:02X}）")
            if p.kind == "DATA":
                check(p.data[0] == 0x2A, f"I2C 数据 0x2A（实际 0x{p.data[0]:02X}）")


def test_spi():
    rate = 1000000
    samples = synth_samples(4000, rate)
    clk, mosi, miso, cs = 0, 1, 2, 3
    # CS 低有效，CPOL=0
    set_bit(samples, cs, 0, 1)
    for k in range(10):
        set_bit(samples, cs, k, 1)
    idx = 10
    for _ in range(10):
        set_bit(samples, cs, idx, 0)
        idx += 1
    # 8 个时钟，MOSI = 0xA5 (1 0 1 0 0 1 0 1, MSB first), MISO = 0x3C
    mosi_bits = [1, 0, 1, 0, 0, 1, 0, 1]
    miso_bits = [0, 0, 1, 1, 1, 1, 0, 0]
    for bit_i in range(8):
        for k in range(6):
            set_bit(samples, mosi, idx + k, mosi_bits[bit_i])
            set_bit(samples, miso, idx + k, miso_bits[bit_i])
        set_bit(samples, clk, idx + 2, 1)
        set_bit(samples, clk, idx + 6, 0)
        idx += 8
    for _ in range(10):
        set_bit(samples, cs, idx, 1)
        idx += 1

    cfg = dec.SpiConfig(clk_ch=clk, mosi_ch=mosi, miso_ch=miso, cs_ch=cs,
                        cs_active=0, cpol=0, cpha=0)
    pkts = dec.decode_spi(samples, rate, cfg)
    check(len(pkts) == 1, f"SPI 解出 1 帧（实际 {len(pkts)}）")
    if pkts:
        check(0xA5 in pkts[0].data, f"SPI MOSI 0xA5（实际 {pkts[0].data.hex()}）")
        check(0x3C in pkts[0].data, "SPI MISO 0x3C")


def test_protocol_frames():
    f = build_frame(0x07, b"\x00\x00\x00\x00\x00\x04\x00\x00")
    check(f[0] == 0xAA and f[1] == 0x55 and f[2] == 0x07, "帧头正确")
    p = FrameParser()
    p.feed(b"\x00\x11" + f)
    frames = list(p.frames())
    check(len(frames) == 1, "带垃圾前缀可重同步")
    check(crc16(f[:-2]) == int.from_bytes(f[-2:], "little"), "CRC 校验通过")


if __name__ == "__main__":
    print("== LogicAnalyzer 解码器测试 ==")
    test_uart()
    test_i2c()
    test_spi()
    test_protocol_frames()
    if failures == 0:
        print("\nALL TESTS PASSED")
        sys.exit(0)
    print(f"\n{failures} TEST(S) FAILED")
    sys.exit(1)
