"""HOSTLINK 协议层主机单元测试（无需第三方依赖，直接 python 运行）"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from vlink.protocol import (
    Command, VarType, calc_crc, build_frame, parse_stream, MIN_FRAME_LEN,
)

failures = 0


def check(cond, msg):
    global failures
    if cond:
        print(f"  ok: {msg}")
    else:
        print(f"  FAIL: {msg}")
        failures += 1


def test_crc_known_vector():
    # AA 55 01 00 00 的 MODBUS CRC-16 = 0x147C（与固件一致）
    check(calc_crc(bytes([0xAA, 0x55, 0x01, 0x00, 0x00])) == 0x147C,
          "CRC-16 已知向量 0x147C")


def test_build_frame():
    frame = build_frame(Command.LIST_VARS)
    check(frame == bytes([0xAA, 0x55, 0x01, 0x00, 0x00, 0x7C, 0x14]),
          "LIST_VARS 空帧与固件一致")

    payload = bytes([0x01, 0x20])
    frame = build_frame(Command.READ_VAR, payload)
    check(frame[0] == 0xAA and frame[1] == 0x55 and frame[2] == 0x04,
          "READ_VAR 帧头正确")
    check(int.from_bytes(frame[3:5], "little") == 2, "payload_len=2")
    check(frame[5:7] == payload, "payload 内容正确")
    check(calc_crc(frame[:-2]) == int.from_bytes(frame[-2:], "little"),
          "帧尾 CRC 正确")


def test_parse_stream_good():
    buf = bytearray(build_frame(Command.LIST_VARS))
    frame = parse_stream(buf)
    check(frame is not None and frame[2] == Command.LIST_VARS,
          "完整帧解析成功")
    check(len(buf) == 0, "解析后缓冲清空")


def test_parse_stream_resync():
    # 前面塞垃圾字节，应自动滑动重同步
    junk = b"\x00\x11\x22" + build_frame(Command.GET_INFO)
    buf = bytearray(junk)
    frame = parse_stream(buf)
    check(frame is not None and frame[2] == Command.GET_INFO,
          "垃圾前缀后重同步成功")


def test_parse_stream_bad_crc():
    good = bytearray(build_frame(Command.READ_VAR, bytes([0x01, 0x20])))
    good[6] ^= 0xFF          # 破坏 CRC
    frame = parse_stream(good)
    check(frame is None, "坏 CRC 帧被丢弃")


def test_parse_stream_partial():
    buf = bytearray(build_frame(Command.LIST_VARS)[:5])  # 不完整
    check(parse_stream(buf) is None, "不完整帧返回 None（等待更多数据）")
    buf += build_frame(Command.LIST_VARS)[5:]
    check(parse_stream(buf) is not None, "补全后解析成功")


def test_enums():
    check(Command.GET_INFO == 0x06 and Command.ERROR == 0xFE,
          "命令码枚举完整")
    check(VarType.FLOAT == 3, "变量类型枚举正确")


if __name__ == "__main__":
    print("== VLink 协议层主机测试 ==")
    test_crc_known_vector()
    test_build_frame()
    test_parse_stream_good()
    test_parse_stream_resync()
    test_parse_stream_bad_crc()
    test_parse_stream_partial()
    test_enums()
    if failures == 0:
        print("\nALL TESTS PASSED")
        sys.exit(0)
    print(f"\n{failures} TEST(S) FAILED")
    sys.exit(1)
