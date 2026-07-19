import struct
from enum import IntEnum

SYNC1 = 0xAA
SYNC2 = 0x55
MIN_FRAME_LEN = 7

class Command(IntEnum):
    LIST_VARS = 0x01
    SUBSCRIBE = 0x02
    DATA      = 0x03
    READ_VAR  = 0x04
    WRITE_VAR = 0x05

class VarType(IntEnum):
    UINT8 = 0
    INT16 = 1
    INT32 = 2
    FLOAT = 3

# 删除原有的 crcmod 导入和 _crc16
def calc_crc(data: bytes) -> int:
    """纯 Python MODBUS CRC-16，多项式 0xA001"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc

def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    header = bytes([SYNC1, SYNC2]) + bytes([cmd])
    length = struct.pack("<H", len(payload))
    data = header + length + payload
    crc = calc_crc(data)
    return data + struct.pack("<H", crc)

def parse_stream(ring_buf: bytearray) -> bytes | None:
    while len(ring_buf) >= MIN_FRAME_LEN:
        while len(ring_buf) >= 2 and (ring_buf[0] != SYNC1 or ring_buf[1] != SYNC2):
            ring_buf.pop(0)
        if len(ring_buf) < MIN_FRAME_LEN:
            return None
        length = struct.unpack("<H", ring_buf[3:5])[0]
        frame_len = 5 + length + 2
        if len(ring_buf) < frame_len:
            return None
        frame_data = bytes(ring_buf[:frame_len])
        del ring_buf[:frame_len]
        crc_received = struct.unpack("<H", frame_data[-2:])[0]
        crc_calc = calc_crc(frame_data[:-2])
        if crc_received == crc_calc:
            return frame_data
        else:
            print(f"CRC mismatch: calc={crc_calc:04X}, recv={crc_received:04X}")
    return None