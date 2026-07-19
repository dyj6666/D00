import struct
from vlink.protocol import VarType

class VariableInfo:
    __slots__ = ('id', 'name', 'type', 'permission')
    def __init__(self, vid: int, name: str, vtype: int, permission: int):
        self.id = vid
        self.name = name
        self.type = vtype
        self.permission = permission

    @property
    def type_name(self) -> str:
        return {0: "uint8", 1: "int16", 2: "int32", 3: "float"}.get(self.type, "unknown")

    def parse_value(self, data: bytes):
        if self.type == VarType.UINT8:
            return data[0]
        if self.type == VarType.INT16:
            return struct.unpack("<h", data[:2])[0]
        if self.type == VarType.INT32:
            return struct.unpack("<i", data[:4])[0]
        if self.type == VarType.FLOAT:
            return struct.unpack("<f", data[:4])[0]
        return None