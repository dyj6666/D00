import struct
import time
from .protocol import Command, VarType, build_frame
from .transport import Transport
from model.variable import VariableInfo

class VLinkClient:
    def __init__(self, transport: Transport):
        self.transport = transport
        self.variables: dict[int, VariableInfo] = {}

    def discover_variables(self) -> list:
        # 清空输入缓冲区，防止残留数据干扰
        self.transport.serial.reset_input_buffer()
        with self.transport.lock:
            self.transport.rx_buf.clear()
        # 清空接收队列
        while not self.transport.rx_queue.empty():
            self.transport.rx_queue.get_nowait()

        self.transport.send(Command.LIST_VARS)
        # 固件 var_list.c 支持多帧分片：payload[0]=总包数，payload[1]=包序号。
        # 必须收齐所有分片再合并，否则变量表超一帧即静默丢变量。
        packets = {}          # packet_index -> 本包解析出的条目
        total_packets = 0     # 固件声明的总包数（0 表示尚未得知）
        deadline = time.time() + 1.0
        while time.time() < deadline:
            frame = self.transport.get_frame(timeout=0.01)
            if frame is None:
                continue
            cmd, payload = frame
            if cmd != Command.LIST_VARS:
                continue
            if len(payload) < 2:
                continue
            total = payload[0]
            index = payload[1]
            if total_packets == 0:
                total_packets = total if total > 0 else 1
            packets[index] = self._parse_variable_list(payload)
            if len(packets) >= total_packets:
                break
        # 按分片序号合并（容忍丢片：保留已收部分，仅提示）
        vars_list = []
        for idx in sorted(packets):
            vars_list.extend(packets[idx])
        if len(packets) > 1 and len(packets) < total_packets:
            print(f"WARN: received {len(packets)}/{total_packets} variable fragments")
        if not packets:
            print("Timeout")
        return vars_list

    def _parse_variable_list(self, payload: bytes) -> list[VariableInfo]:
        # print(f"Parsing variable list, payload: {payload.hex()}")
            # 跳过分片信息（2字节）
        if len(payload) < 2:
            return []
        payload = payload[2:]
        
        vars_list = []
        idx = 0
        while idx + 5 <= len(payload):
            vid = struct.unpack("<H", payload[idx:idx+2])[0]
            vtype = payload[idx+2]
            perm = payload[idx+3]
            name_len = payload[idx+4]
            idx += 5
            if idx + name_len > len(payload):
                break
            name = payload[idx:idx+name_len].decode("ascii", errors="replace")
            idx += name_len
            var = VariableInfo(vid, name, vtype, perm)
            vars_list.append(var)
            self.variables[vid] = var
        return vars_list

    def subscribe(self, var_ids: list[int]):
        payload = b"".join(struct.pack("<H", vid) for vid in var_ids)
        self.transport.send(Command.SUBSCRIBE, payload)

    def read_variable(self, vid: int):
        self.transport.send(Command.READ_VAR, struct.pack("<H", vid))

    def write_variable(self, vid: int, value):
        var = self.variables.get(vid)
        if var is None:
            return
        if var.type == VarType.UINT8:
            val_bytes = bytes([value & 0xFF])
        elif var.type == VarType.INT16:
            val_bytes = struct.pack("<h", value)
        elif var.type == VarType.INT32:
            val_bytes = struct.pack("<i", value)
        elif var.type == VarType.FLOAT:
            val_bytes = struct.pack("<f", value)
        else:
            return
        payload = struct.pack("<H", vid) + bytes([len(val_bytes)]) + val_bytes
        self.transport.send(Command.WRITE_VAR, payload)

    def get_data_frames(self, timeout=0.01) -> list[tuple[int, float]]:
        results = []
        while True:
            frame = self.transport.get_frame(timeout=timeout)
            if frame is None:
                break
            cmd, payload = frame
            if cmd == Command.DATA:
                idx = 0
                while idx + 4 <= len(payload):
                    vid = struct.unpack("<H", payload[idx:idx+2])[0]
                    val_len = payload[idx+2]
                    idx += 4
                    if idx + val_len > len(payload):
                        break
                    val_bytes = payload[idx:idx+val_len]
                    idx += val_len
                    var = self.variables.get(vid)
                    if var:
                        value = var.parse_value(val_bytes)
                        results.append((vid, value))
        return results
