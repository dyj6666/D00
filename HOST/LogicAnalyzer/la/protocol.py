"""HOSTLINK 协议帧工具（与 MCU 端 SystemServices/protocol.c 对齐）"""
import struct

SYNC1 = 0xAA
SYNC2 = 0x55

CMD_LIST_VARS = 0x01
CMD_SUBSCRIBE = 0x02
CMD_DATA      = 0x03
CMD_READ_VAR  = 0x04
CMD_WRITE_VAR = 0x05
CMD_GET_INFO  = 0x06
CMD_LA_DUMP   = 0x07


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    body = bytes([SYNC1, SYNC2, cmd]) + struct.pack("<H", len(payload)) + payload
    return body + struct.pack("<H", crc16(body))


def build_la_dump_request(offset: int, count: int) -> bytes:
    return build_frame(CMD_LA_DUMP, struct.pack("<II", offset, count))


class FrameParser:
    """流式帧解析：从字节流中提取完整合法帧（自动重同步）"""

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes):
        self._buf += data

    def frames(self):
        while len(self._buf) >= 7:
            if self._buf[0] != SYNC1 or self._buf[1] != SYNC2:
                self._buf.pop(0)
                continue
            plen = struct.unpack("<H", bytes(self._buf[3:5]))[0]
            flen = 5 + plen + 2
            if len(self._buf) < flen:
                return
            frame = bytes(self._buf[:flen])
            del self._buf[:flen]
            if crc16(frame[:-2]) == struct.unpack("<H", frame[-2:])[0]:
                yield frame
