"""HOSTLINK 协议帧工具：与 APP SystemServices/protocol.h 对齐。

用于 APP 运行时 OTA（固件经 HOSTLINK 下载到 DOWNLOAD 区），
支持 CMD_OTA_BEGIN/DATA/END/STATUS（0x08~0x0B）。
"""
from __future__ import annotations

import struct

SYNC1 = 0xAA
SYNC2 = 0x55

CMD_OTA_BEGIN = 0x08
CMD_OTA_DATA = 0x09
CMD_OTA_END = 0x0A
CMD_OTA_STATUS = 0x0B

OTA_CHUNK_MAX = 240


def crc16(data: bytes) -> int:
    """CRC-16/MODBUS，与固件 crc16.c 一致。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    body = bytes([SYNC1, SYNC2, cmd]) + struct.pack("<H", len(payload)) + payload
    return body + struct.pack("<H", crc16(body))


def build_ota_begin(version: int, size: int) -> bytes:
    return build_frame(CMD_OTA_BEGIN, struct.pack("<II", version, size))


def build_ota_data(offset: int, chunk: bytes) -> bytes:
    assert len(chunk) <= OTA_CHUNK_MAX
    return build_frame(CMD_OTA_DATA, struct.pack("<I", offset) + chunk)


def build_ota_end() -> bytes:
    return build_frame(CMD_OTA_END)


def build_ota_status() -> bytes:
    return build_frame(CMD_OTA_STATUS)


class FrameParser:
    """流式帧解析：自动重同步、CRC 校验。"""

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

    @staticmethod
    def payload(frame: bytes) -> bytes:
        """取帧载荷（不含同步/命令/长度/CRC）。"""
        plen = struct.unpack("<H", frame[3:5])[0]
        return frame[5:5 + plen]
