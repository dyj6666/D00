"""三协议自动化鲁棒性测试：UART/SPI/I2C 各 20 组实机帧验证。

运行：python tests/test_robustness.py
前置：固件已烧录（含 signal_gen），PC6/PB13/PB15/PB12/PE2/PE3 按协议接线，
      串口 COM9(控制) / COM13(数据) 空闲。
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import serial

from la.capture import CaptureSession
from la import decoders as dec

CTRL = "COM9"
DATA = "COM13"


def send(cmd: str, wait: float = 0.5) -> str:
    s = serial.Serial(CTRL, 115200, timeout=0.8)
    time.sleep(0.1)
    s.reset_input_buffer()
    s.write(cmd.encode() + b"\r")
    time.sleep(wait)
    data = s.read(1024).decode("utf-8", errors="replace")
    s.close()
    return data.strip()


def stop_all():
    for cmd in ("sg_uart_stop", "sg_spi_stop", "sg_i2c_stop"):
        send(cmd, wait=0.2)


results = []


def run_case(name, setup_cmd, rate, proto, verify, note=""):
    stop_all()
    time.sleep(0.2)
    resp = send(setup_cmd)
    if "STARTED" not in resp and "COMPLEX" not in resp:
        results.append((name, False, f"发生器启动失败: {resp}"))
        return
    sess = CaptureSession(CTRL, DATA)
    try:
        trace = sess.capture(rate=rate, duration_s=0.3, buffer_src="sram",
                             count=32768, nchannels=4)
    finally:
        sess.close()
    try:
        ok, detail = verify(trace, proto)
    except Exception as e:  # noqa: BLE001
        ok, detail = False, f"异常: {e}"
    results.append((name, ok, detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}")


def verify_uart(trace, baud, expected):
    cfg = dec.UartConfig(tx_ch=0, rx_ch=None, baud=baud)
    pkts = dec.decode_uart(trace.samples, trace.rate, cfg)
    data = bytes(p.data[0] for p in pkts if p.data)
    if len(data) < len(expected):
        return False, f"字节不足 {len(data)}<{len(expected)}"
    ok = data[:len(expected)] == expected
    return ok, f"解出 {data[:len(expected)].hex(' ')} vs 期望 {expected.hex(' ')}"


def verify_spi(trace, expected):
    cfg = dec.SpiConfig(clk_ch=0, mosi_ch=1, miso_ch=3, cs_ch=2,
                        cs_active=0, cpol=0, cpha=0, lsb_first=False)
    pkts = dec.decode_spi(trace.samples, trace.rate, cfg)
    data = bytes(p.data[0] for p in pkts if len(p.data) >= 1)
    if len(data) < len(expected):
        return False, f"字节不足 {len(data)}<{len(expected)}"
    ok = data[:len(expected)] == expected
    return ok, f"解出 {data[:len(expected)].hex(' ')} vs 期望 {expected.hex(' ')}"


def verify_i2c(trace, addr, expected_data, expect_rw_nack=None):
    cfg = dec.I2cConfig(scl_ch=0, sda_ch=1)
    pkts = dec.decode_i2c(trace.samples, trace.rate, cfg)
    addrs = [p for p in pkts if p.kind == "ADDR"]
    datas = [p for p in pkts if p.kind == "DATA"]
    if not addrs:
        return False, "无 ADDR"
    a_ok = any(f"0x{addr:02X}" in p.info for p in addrs)
    dbytes = bytes(p.data[0] for p in datas if p.data)
    d_ok = len(dbytes) >= len(expected_data) and \
        dbytes[:len(expected_data)] == expected_data
    detail = f"ADDR ok={a_ok} DATA={dbytes[:len(expected_data)].hex(' ')}"
    return a_ok and d_ok, detail


def main():
    import sys as _sys
    only = _sys.argv[1].lower() if len(_sys.argv) > 1 else "all"

    # ============ UART 20 组 ============
    uart_cases = []
    uart_cases += [("UART-9600-HELLO", "sg_uart_start 9600 HELLO 5",
                    1000000, 9600, b"HELLO")]
    uart_cases += [("UART-19200-HELLO", "sg_uart_start 19200 HELLO 5",
                    1000000, 19200, b"HELLO")]
    uart_cases += [("UART-38400-HELLO", "sg_uart_start 38400 HELLO 5",
                    1000000, 38400, b"HELLO")]
    uart_cases += [("UART-57600-HELLO", "sg_uart_start 57600 HELLO 5",
                    1000000, 57600, b"HELLO")]
    uart_cases += [("UART-115200-HELLO", "sg_uart_start 115200 HELLO 5",
                    1000000, 115200, b"HELLO")]
    uart_cases += [("UART-115200-hex", "sg_uart_hex 115200 AA55010203 5",
                    1000000, 115200, bytes.fromhex("AA55010203"))]
    uart_cases += [("UART-115200-boundary",
                    "sg_uart_hex 115200 00FF7F800D0A 5",
                    1000000, 115200, bytes.fromhex("00FF7F800D0A"))]
    uart_cases += [("UART-115200-long",
                    "sg_uart_hex 115200 48656C6C6F576F726C6454657374313233 5",
                    1000000, 115200, b"HelloWorldTest123")]
    uart_cases += [("UART-230400-hex", "sg_uart_hex 230400 A5A5C3C3 5",
                    1000000, 230400, bytes.fromhex("A5A5C3C3"))]
    uart_cases += [("UART-460800-hex", "sg_uart_hex 460800 0102030405 5",
                    2000000, 460800, bytes.fromhex("0102030405"))]
    uart_cases += [("UART-921600-hex", "sg_uart_hex 921600 0F0E0D0C 5",
                    5000000, 921600, bytes.fromhex("0F0E0D0C"))]
    uart_cases += [("UART-9600-boundary", "sg_uart_hex 9600 FF00FF00 5",
                    1000000, 9600, bytes.fromhex("FF00FF00"))]
    uart_cases += [("UART-19200-hex", "sg_uart_hex 19200 55667788 5",
                    1000000, 19200, bytes.fromhex("55667788"))]
    uart_cases += [("UART-57600-boundary", "sg_uart_hex 57600 807F8080 5",
                    1000000, 57600, bytes.fromhex("807F8080"))]
    uart_cases += [("UART-115200-repeat", "sg_uart_hex 115200 DEADBEEF 3",
                    1000000, 115200, bytes.fromhex("DEADBEEF"))]
    uart_cases += [("UART-115200-single00", "sg_uart_hex 115200 00 5",
                    1000000, 115200, b"\x00")]
    uart_cases += [("UART-115200-singleFF", "sg_uart_hex 115200 FF 5",
                    1000000, 115200, b"\xff")]
    uart_cases += [("UART-230400-boundary", "sg_uart_hex 230400 00112233 5",
                    1000000, 230400, bytes.fromhex("00112233"))]
    uart_cases += [("UART-460800-boundary", "sg_uart_hex 460800 FF000102 5",
                    2000000, 460800, bytes.fromhex("FF000102"))]
    uart_cases += [("UART-921600-8byte", "sg_uart_hex 921600 0102030405060708 5",
                    5000000, 921600, bytes.fromhex("0102030405060708"))]
    if only in ("all", "uart"):
        for name, cmd, rate, baud, exp in uart_cases:
            run_case(name, cmd, rate, "UART",
                     lambda t, p, b=baud, e=exp: verify_uart(t, b, e))

    # ============ SPI 20 组 ============
    spi_cases = [
        ("SPI-A53C55AAFF00", "sg_spi_start A53C55AAFF00 2", bytes.fromhex("A53C55AAFF00")),
        ("SPI-102030405060", "sg_spi_start 102030405060 2", bytes.fromhex("102030405060")),
        ("SPI-FF000102", "sg_spi_start FF000102 2", bytes.fromhex("FF000102")),
        ("SPI-DEADBEEF", "sg_spi_start DEADBEEF 2", bytes.fromhex("DEADBEEF")),
        ("SPI-00FF7F80", "sg_spi_start 00FF7F80 2", bytes.fromhex("00FF7F80")),
        ("SPI-1234567890AB", "sg_spi_start 1234567890AB 2", bytes.fromhex("1234567890AB")),
        ("SPI-FFEEDDCC", "sg_spi_start FFEEDDCC 2", bytes.fromhex("FFEEDDCC")),
        ("SPI-000000", "sg_spi_start 000000 2", bytes.fromhex("000000")),
        ("SPI-FFFFFF", "sg_spi_start FFFFFF 2", bytes.fromhex("FFFFFF")),
        ("SPI-5A5A5A5A", "sg_spi_start 5A5A5A5A 2", bytes.fromhex("5A5A5A5A")),
        ("SPI-A53C55AAFF00-5ms", "sg_spi_start A53C55AAFF00 5", bytes.fromhex("A53C55AAFF00")),
        ("SPI-102030-5ms", "sg_spi_start 102030 5", bytes.fromhex("102030")),
        ("SPI-FF00-5ms", "sg_spi_start FF00 5", bytes.fromhex("FF00")),
        ("SPI-DEAD-5ms", "sg_spi_start DEAD 5", bytes.fromhex("DEAD")),
        ("SPI-0102030405060708", "sg_spi_start 0102030405060708 2", bytes.fromhex("0102030405060708")),
        ("SPI-8899AABB", "sg_spi_start 8899AABB 2", bytes.fromhex("8899AABB")),
        ("SPI-000102030405", "sg_spi_start 000102030405 2", bytes.fromhex("000102030405")),
        ("SPI-FEFEFEFE", "sg_spi_start FEFEFEFE 2", bytes.fromhex("FEFEFEFE")),
        ("SPI-1F2E3D4C", "sg_spi_start 1F2E3D4C 2", bytes.fromhex("1F2E3D4C")),
        ("SPI-CAFE1234", "sg_spi_start CAFE1234 2", bytes.fromhex("CAFE1234")),
    ]
    if only in ("all", "spi"):
        for name, cmd, exp in spi_cases:
            run_case(name, cmd, 5000000, "SPI",
                     lambda t, p, e=exp: verify_spi(t, e))

    # ============ I2C 20 组 ============
    i2c_cases = [
        ("I2C-50-AA55010203", "sg_i2c_start 50 AA55010203 5", 0x50, bytes.fromhex("AA55010203")),
        ("I2C-55-DEADBEEF", "sg_i2c_start 55 DEADBEEF 5", 0x55, bytes.fromhex("DEADBEEF")),
        ("I2C-00-00FF7F80", "sg_i2c_start 00 00FF7F80 5", 0x00, bytes.fromhex("00FF7F80")),
        ("I2C-7F-A5", "sg_i2c_start 7F A5 5", 0x7F, b"\xA5"),
        ("I2C-50-00", "sg_i2c_start 50 00 5", 0x50, b"\x00"),
        ("I2C-50-FF", "sg_i2c_start 50 FF 5", 0x50, b"\xff"),
        ("I2C-50-112233", "sg_i2c_start 50 112233 5", 0x50, bytes.fromhex("112233")),
        ("I2C-50-12345678", "sg_i2c_start 50 12345678 5", 0x50, bytes.fromhex("12345678")),
        ("I2C-2A-ABCDEF", "sg_i2c_start 2A ABCDEF 5", 0x2A, bytes.fromhex("ABCDEF")),
        ("I2C-50-FFEEDDCC", "sg_i2c_start 50 FFEEDDCC 5", 0x50, bytes.fromhex("FFEEDDCC")),
        ("I2C-50-00010203", "sg_i2c_start 50 00010203 5", 0x50, bytes.fromhex("00010203")),
        ("I2C-50-80818283", "sg_i2c_start 50 80818283 5", 0x50, bytes.fromhex("80818283")),
        ("I2C-3C-556677", "sg_i2c_start 3C 556677 5", 0x3C, bytes.fromhex("556677")),
        ("I2C-50-AA", "sg_i2c_start 50 AA 5", 0x50, b"\xAA"),
        ("I2C-50-55", "sg_i2c_start 50 55 5", 0x50, b"\x55"),
        ("I2C-60-A0B0C0D0", "sg_i2c_start 60 A0B0C0D0 5", 0x60, bytes.fromhex("A0B0C0D0")),
        ("I2C-50-1234", "sg_i2c_start 50 1234 5", 0x50, bytes.fromhex("1234")),
        ("I2C-50-ABCD", "sg_i2c_start 50 ABCD 5", 0x50, bytes.fromhex("ABCD")),
        ("I2C-5A-complex", "sg_i2c_complex 5A 5", 0x5A, bytes.fromhex("AA550102")),
        ("I2C-50-complex", "sg_i2c_complex 50 5", 0x50, bytes.fromhex("AA550102")),
    ]
    if only in ("all", "i2c"):
        for name, cmd, addr, exp in i2c_cases:
            run_case(name, cmd, 1000000, "I2C",
                     lambda t, p, a=addr, e=exp: verify_i2c(t, a, e))

    # ============ 汇总 ============
    passed = sum(1 for _, ok, _ in results if ok)
    print()
    print("=" * 60)
    print(f"总计 {len(results)} 组，通过 {passed}，失败 {len(results) - passed}")
    for name, ok, detail in results:
        if not ok:
            print(f"  FAIL {name}: {detail}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
