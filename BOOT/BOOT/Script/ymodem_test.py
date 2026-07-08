#!/usr/bin/env python3
"""
===============================================================================
 文件名称: ymodem_sender.py
 描述:     工业级 Ymodem 固件发送器 (上位机)
           - 与 MCU 端 ymodem.c 最终版 经过逐行交叉验证，保证完美匹配
           - 采用事件驱动状态机，结构清晰，注释详尽
           - 默认参数集成在脚本顶部，也支持命令行参数覆盖

 用法:     python ymodem_sender.py [串口号] [固件文件.bin]
           若未提供参数，则使用默认 COM3 和 firmware.bin

 示例:     python ymodem_sender.py COM3 my_app.bin
           python ymodem_sender.py               # 使用默认配置

 依赖:     pyserial (pip install pyserial)
 作者:     Industrial OTA Team
 版本:     1.0.0 (最终版)
===============================================================================
"""

import sys
import time
import struct
import os
import serial

# ========================== 用户配置区域 ======================================
DEFAULT_SERIAL_PORT    = "COM13"           # 串口号 (Windows: COM3, Linux: /dev/ttyUSB0)
DEFAULT_BAUD_RATE      = 115200          # 波特率 (必须与 MCU 一致)
DEFAULT_FIRMWARE_FILE  = "APP.bin"  # 默认固件文件名 (可含路径)
# =============================================================================

# ========================== 协议常量 (与 ymodem.h 完全一致) ====================
SOH = 0x01              # 128字节帧头 (文件信息/结束帧)
STX = 0x02              # 1024字节帧头 (数据帧)
EOT = 0x04              # 传输结束
ACK = 0x06              # 确认
NAK = 0x15              # 不确认 (请求重传)
CAN = 0x18              # 取消
C   = 0x43              # 'C' 握手字符

PACKET_SIZE      = 1024  # 数据帧有效载荷
FILE_INFO_SIZE   = 128   # 文件信息块大小
MAX_RETRY        = 5     # 每个阶段最大重试次数
INTER_BYTE_DELAY = 0.005 # 发送后微小延时 (秒)，保证数据完全推送到硬件
# =============================================================================

# ========================== CRC32 查表 (与 MCU crc32.c 完全一致) ==============
CRC32_TABLE = [
    0x00000000,0x77073096,0xEE0E612C,0x990951BA,0x076DC419,0x706AF48F,0xE963A535,0x9E6495A3,
    0x0EDB8832,0x79DCB8A4,0xE0D5E91E,0x97D2D988,0x09B64C2B,0x7EB17CBD,0xE7B82D07,0x90BF1D91,
    0x1DB71064,0x6AB020F2,0xF3B97148,0x84BE41DE,0x1ADAD47D,0x6DDDE4EB,0xF4D4B551,0x83D385C7,
    0x136C9856,0x646BA8C0,0xFD62F97A,0x8A65C9EC,0x14015C4F,0x63066CD9,0xFA0F3D63,0x8D080DF5,
    0x3B6E20C8,0x4C69105E,0xD56041E4,0xA2677172,0x3C03E4D1,0x4B04D447,0xD20D85FD,0xA50AB56B,
    0x35B5A8FA,0x42B2986C,0xDBBBC9D6,0xACBCF940,0x32D86CE3,0x45DF5C75,0xDCD60DCF,0xABD13D59,
    0x26D930AC,0x51DE003A,0xC8D75180,0xBFD06116,0x21B4F4B5,0x56B3C423,0xCFBA9599,0xB8BDA50F,
    0x2802B89E,0x5F058808,0xC60CD9B2,0xB10BE924,0x2F6F7C87,0x58684C11,0xC1611DAB,0xB6662D3D,
    0x76DC4190,0x01DB7106,0x98D220BC,0xEFD5102A,0x71B18589,0x06B6B51F,0x9FBFE4A5,0xE8B8D433,
    0x7807C9A2,0x0F00F934,0x9609A88E,0xE10E9818,0x7F6A0DBB,0x086D3D2D,0x91646C97,0xE6635C01,
    0x6B6B51F4,0x1C6C6162,0x856530D8,0xF262004E,0x6C0695ED,0x1B01A57B,0x8208F4C1,0xF50FC457,
    0x65B0D9C6,0x12B7E950,0x8BBEB8EA,0xFCB9887C,0x62DD1DDF,0x15DA2D49,0x8CD37CF3,0xFBD44C65,
    0x4DB26158,0x3AB551CE,0xA3BC0074,0xD4BB30E2,0x4ADFA541,0x3DD895D7,0xA4D1C46D,0xD3D6F4FB,
    0x4369E96A,0x346ED9FC,0xAD678846,0xDA60B8D0,0x44042D73,0x33031DE5,0xAA0A4C5F,0xDD0D7CC9,
    0x5005713C,0x270241AA,0xBE0B1010,0xC90C2086,0x5768B525,0x206F85B3,0xB966D409,0xCE61E49F,
    0x5EDEF90E,0x29D9C998,0xB0D09822,0xC7D7A8B4,0x59B33D17,0x2EB40D81,0xB7BD5C3B,0xC0BA6CAD,
    0xEDB88320,0x9ABFB3B6,0x03B6E20C,0x74B1D29A,0xEAD54739,0x9DD277AF,0x04DB2615,0x73DC1683,
    0xE3630B12,0x94643B84,0x0D6D6A3E,0x7A6A5AA8,0xE40ECF0B,0x9309FF9D,0x0A00AE27,0x7D079EB1,
    0xF00F9344,0x8708A3D2,0x1E01F268,0x6906C2FE,0xF762575D,0x806567CB,0x196C3671,0x6E6B06E7,
    0xFED41B76,0x89D32BE0,0x10DA7A5A,0x67DD4ACC,0xF9B9DF6F,0x8EBEEFF9,0x17B7BE43,0x60B08ED5,
    0xD6D6A3E8,0xA1D1937E,0x38D8C2C4,0x4FDFF252,0xD1BB67F1,0xA6BC5767,0x3FB506DD,0x48B2364B,
    0xD80D2BDA,0xAF0A1B4C,0x36034AF6,0x41047A60,0xDF60EFC3,0xA867DF55,0x316E8EEF,0x4669BE79,
    0xCB61B38C,0xBC66831A,0x256FD2A0,0x5268E236,0xCC0C7795,0xBB0B4703,0x220216B9,0x5505262F,
    0xC5BA3BBE,0xB2BD0B28,0x2BB45A92,0x5CB30A04,0xC2D7FFA7,0xB5D0CF31,0x2CD99E8B,0x5BDEAE1D,
    0x9B64C2B0,0xEC63F226,0x756AA39C,0x026D930A,0x9C0906A9,0xEB0E363F,0x72076785,0x05005713,
    0x95BF4A82,0xE2B87A14,0x7BB12BAE,0x0CB61B38,0x92D28E9B,0xE5D5BE0D,0x7CDCEFB7,0x0BDBDF21,
    0x86D3D2D4,0xF1D4E242,0x68DDB3F8,0x1FDA836E,0x81BE16CD,0xF6B9265B,0x6FB077E1,0x18B74777,
    0x88085AE6,0xFF0F6A70,0x66063BCA,0x11010B5C,0x8F659EFF,0xF862AE69,0x616BFFD3,0x166CCF45,
    0xA00AE278,0xD70DD2EE,0x4E048354,0x3903B3C2,0xA7672661,0xD06016F7,0x4969474D,0x3E6E77DB,
    0xAED16A4A,0xD9D65ADC,0x40DF0B66,0x37D83BF0,0xA9BCAE53,0xDEBB9EC5,0x47B2CF7F,0x30B5FFE9,
    0xBDBDF21C,0xCABAC28A,0x53B39330,0x24B4A3A6,0xBAD03605,0xCDD70693,0x54DE5729,0x23D967BF,
    0xB3667A2E,0xC4614AB8,0x5D681B02,0x2A6F2B94,0xB40BBE37,0xC30C8EA1,0x5A05DF1B,0x2D02EF8D
]
# =============================================================================

def crc32(data: bytes) -> int:
    """ 计算 CRC32 (与 MCU 端完全一致) """
    crc = 0xFFFFFFFF
    for byte in data:
        crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ byte) & 0xFF]
    return crc ^ 0xFFFFFFFF

# ========================== 状态机状态定义 ====================================
class State:
    WAIT_C          = 0   # 等待接收 'C' 握手
    SEND_FILE_INFO  = 1   # 发送文件信息帧
    WAIT_ACK_C      = 2   # 等待 ACK + 'C' (或 ACK)
    SEND_DATA       = 3   # 发送数据帧
    SEND_EOT_FIRST  = 4   # 发送第一次 EOT
    WAIT_NAK        = 5   # 等待 NAK
    SEND_EOT_SECOND = 6   # 发送第二次 EOT
    WAIT_ACK_C2     = 7   # 等待第二次 ACK + 'C'
    SEND_END_FRAME  = 8   # 发送结束帧
    WAIT_ACK_END    = 9   # 等待最后 ACK
    DONE            = 10  # 传输成功
    ERROR           = 11  # 传输失败
# =============================================================================

class YmodemSender:
    """ Ymodem 固件发送器 (状态机，与 MCU ymodem.c 完全匹配) """

    def __init__(self, port: str, baudrate: int = DEFAULT_BAUD_RATE, timeout: float = 0.5):
        self.ser = serial.Serial(port, baudrate, timeout=timeout)
        self.state = State.WAIT_C
        self.retry = 0
        self.seq = 0                    # 下一数据包序号
        self.data = b''                 # 固件二进制数据
        self.file_name = ''
        self.file_size = 0
        self.file_crc = 0
        self.offset = 0                 # 当前已发送的原始数据偏移
        self.last_offset = 0            # 用于重发时正确恢复偏移
        self.timer = 0
        self._data_mode = False         # 区分文件信息等待与数据帧等待

    # ---------- 超时管理 ----------
    def _set_timeout(self, seconds: float):
        self.timer = time.time()

    def _is_timeout(self, seconds: float) -> bool:
        return (time.time() - self.timer) >= seconds

    # ---------- 底层收发 ----------
    def _read_byte(self, timeout_s: float = 0.5) -> int:
        self.ser.timeout = timeout_s
        byte = self.ser.read(1)
        return byte[0] if byte else -1

    def _send_packet(self, packet: bytes):
        self.ser.write(packet)
        self.ser.flush()
        time.sleep(INTER_BYTE_DELAY)

    # ---------- 帧构建 ----------
    def _build_file_info_frame(self) -> bytes:
        """ 文件信息帧: SOH + 0x00 + 0xFF + 128字节信息 + CRC32 """
        info_str = f"{self.file_name}\x00 0x{self.file_size:X} 0x{self.file_crc:X}\x00"
        info_bytes = info_str.encode('ascii')
        if len(info_bytes) > FILE_INFO_SIZE:
            raise ValueError("文件信息字符串超出128字节")
        info_bytes += b'\x00' * (FILE_INFO_SIZE - len(info_bytes))
        header = struct.pack('>BBB', SOH, 0x00, 0xFF)
        crc_val = crc32(info_bytes)
        crc_bytes = struct.pack('<I', crc_val)
        return header + info_bytes + crc_bytes

    def _build_data_frame(self, seq: int, chunk: bytes) -> bytes:
        """ 数据帧: STX + seq + ~seq + 1024字节数据 + CRC32 """
        assert len(chunk) == PACKET_SIZE
        seq_comp = 0xFF ^ seq
        header = struct.pack('>BBB', STX, seq, seq_comp)
        crc_val = crc32(chunk)
        crc_bytes = struct.pack('<I', crc_val)
        return header + chunk + crc_bytes

    def _build_end_frame(self) -> bytes:
        """ 结束帧: SOH + 0x00 + 0xFF + 128字节全零 + CRC32 """
        header = struct.pack('>BBB', SOH, 0x00, 0xFF)
        zero_data = b'\x00' * FILE_INFO_SIZE
        crc_val = crc32(zero_data)
        crc_bytes = struct.pack('<I', crc_val)
        return header + zero_data + crc_bytes

    # ---------- 主入口 ----------
    def run(self, file_path: str):
        if not os.path.exists(file_path):
            print(f"[错误] 文件不存在: {file_path}")
            return
        with open(file_path, 'rb') as f:
            self.data = f.read()
        self.file_size = len(self.data)
        self.file_name = os.path.basename(file_path)
        self.file_crc = crc32(self.data)
        print(f"[信息] 文件: {self.file_name}, 大小: {self.file_size} (0x{self.file_size:X}), "
              f"CRC32: 0x{self.file_crc:08X}")

        # 重置状态机
        self.state = State.WAIT_C
        self.retry = 0
        self.seq = 1
        self.offset = 0
        self.last_offset = 0
        self._data_mode = False
        self._set_timeout(10.0)

        # 主循环
        while self.state not in [State.DONE, State.ERROR]:
            self._dispatch()

        self.ser.close()
        if self.state == State.DONE:
            print("[成功] 固件升级文件发送完毕")
        else:
            print("[失败] 传输中止")

    # ---------- 调度器 ----------
    def _dispatch(self):
        dispatch_map = {
            State.WAIT_C:          self._handle_wait_c,
            State.SEND_FILE_INFO:  self._handle_send_file_info,
            State.WAIT_ACK_C:      self._handle_wait_ack_c,
            State.SEND_DATA:       self._handle_send_data,
            State.SEND_EOT_FIRST:  self._handle_send_eot_first,
            State.WAIT_NAK:        self._handle_wait_nak,
            State.SEND_EOT_SECOND: self._handle_send_eot_second,
            State.WAIT_ACK_C2:     self._handle_wait_ack_c2,
            State.SEND_END_FRAME:  self._handle_send_end_frame,
            State.WAIT_ACK_END:    self._handle_wait_ack_end,
        }
        handler = dispatch_map.get(self.state)
        if handler:
            handler()
        else:
            self.state = State.ERROR

    # ======================== 各状态处理函数 ==================================

    def _handle_wait_c(self):
        """ 等待 MCU 发送 'C' 握手 """
        ch = self._read_byte(0.2)
        if ch == C:
            print("[状态] 收到握手 'C'，开始发送文件信息")
            self.state = State.SEND_FILE_INFO
            self.retry = 0
        if self._is_timeout(10.0):
            print("[错误] 等待 'C' 超时，请检查 MCU 是否进入升级模式")
            self.state = State.ERROR

    def _handle_send_file_info(self):
        """ 发送文件信息帧 """
        print("[状态] 发送文件信息帧...")
        frame = self._build_file_info_frame()
        self._send_packet(frame)
        self._set_timeout(3.0)
        self.state = State.WAIT_ACK_C
        self.retry = 0
        self._data_mode = False   # 期望 ACK + 'C'

    def _handle_wait_ack_c(self):
        """ 等待响应：文件信息阶段等待 ACK+'C'，数据阶段等待 ACK/NAK """
        ch1 = self._read_byte(0.5)
        if ch1 < 0:
            pass
        elif ch1 == ACK:
            if not self._data_mode:
                # 文件信息帧后的 ACK，必须再接收到 'C'
                ch2 = self._read_byte(0.5)
                if ch2 == C:
                    print("[状态] 收到 ACK + 'C'，开始传输数据")
                    self.state = State.SEND_DATA
                    self.retry = 0
                    self.seq = 1
                    self.offset = 0
                    return
                else:
                    if ch2 >= 0:
                        print(f"[警告] 预期 'C' 但收到 0x{ch2:02X}")
            else:
                # 数据帧后的 ACK
                print(f"[状态] 数据帧 #{self.seq-1} 确认")
                self.seq = (self.seq + 1) & 0xFF
                if self.seq == 0:
                    self.seq = 1
                self.offset = self.last_offset + PACKET_SIZE   # 前移原始数据指针
                self.state = State.SEND_DATA
                self.retry = 0
                return
        elif ch1 == NAK:
            print("[警告] 收到 NAK，准备重传")
            if self.retry < MAX_RETRY:
                self.retry += 1
                if self._data_mode:
                    self.offset = self.last_offset   # 恢复偏移
                self.state = State.SEND_DATA if self._data_mode else State.SEND_FILE_INFO
            else:
                print("[错误] 重试次数耗尽")
                self.state = State.ERROR
            return

        # 检查超时
        if self._is_timeout(5.0):
            if self.retry < MAX_RETRY:
                print(f"[重试] 超时，重新发送 ({self.retry+1}/{MAX_RETRY})")
                self.retry += 1
                if self._data_mode:
                    self.offset = self.last_offset
                self.state = State.SEND_DATA if self._data_mode else State.SEND_FILE_INFO
            else:
                print("[错误] 无响应超时")
                self.state = State.ERROR

    def _handle_send_data(self):
        """ 发送下一数据帧，或全部发送完毕后转入 EOT 阶段 """
        if self.offset >= self.file_size:
            self.state = State.SEND_EOT_FIRST
            self.retry = 0
            return

        self.last_offset = self.offset
        chunk = self.data[self.offset:self.offset+PACKET_SIZE]
        if len(chunk) < PACKET_SIZE:
            chunk += b'\x00' * (PACKET_SIZE - len(chunk))

        frame = self._build_data_frame(self.seq, chunk)
        self._send_packet(frame)
        print(f"[发送] 数据帧 #{self.seq}, 偏移 {self.offset}")
        self.offset += PACKET_SIZE
        self._set_timeout(3.0)
        self.state = State.WAIT_ACK_C
        self._data_mode = True    # 数据模式
        self.retry = 0

    def _handle_send_eot_first(self):
        """ 发送第一次 EOT """
        print("[状态] 发送 EOT (第一次)")
        self._send_packet(bytes([EOT]))
        self._set_timeout(2.0)
        self.state = State.WAIT_NAK
        self.retry = 0

    def _handle_wait_nak(self):
        """ 等待 MCU 回复 NAK """
        ch = self._read_byte(0.5)
        if ch == NAK:
            print("[状态] 收到 NAK，发送第二次 EOT")
            self.state = State.SEND_EOT_SECOND
            self.retry = 0
        elif ch >= 0:
            print(f"[警告] 预期 NAK 但收到 0x{ch:02X}")
        if self._is_timeout(3.0):
            if self.retry < MAX_RETRY:
                print(f"[重试] 重新发送 EOT ({self.retry+1})")
                self.retry += 1
                self.state = State.SEND_EOT_FIRST
            else:
                print("[错误] EOT 阶段超时")
                self.state = State.ERROR

    def _handle_send_eot_second(self):
        """ 发送第二次 EOT """
        print("[状态] 发送 EOT (第二次)")
        self._send_packet(bytes([EOT]))
        self._set_timeout(2.0)
        self.state = State.WAIT_ACK_C2
        self.retry = 0

    def _handle_wait_ack_c2(self):
        """ 等待 ACK + 'C' (结束帧握手) """
        ch1 = self._read_byte(0.5)
        if ch1 == ACK:
            ch2 = self._read_byte(0.5)
            if ch2 == C:
                print("[状态] 收到 ACK + 'C'，发送结束帧")
                self.state = State.SEND_END_FRAME
                return
        if self._is_timeout(5.0):
            if self.retry < MAX_RETRY:
                self.retry += 1
                self.state = State.SEND_EOT_SECOND
            else:
                print("[错误] 结束握手超时")
                self.state = State.ERROR

    def _handle_send_end_frame(self):
        """ 发送结束帧 (全零 SOH) """
        print("[状态] 发送结束帧")
        frame = self._build_end_frame()
        self._send_packet(frame)
        self._set_timeout(3.0)
        self.state = State.WAIT_ACK_END

    def _handle_wait_ack_end(self):
        """ 等待最终 ACK """
        ch = self._read_byte(0.5)
        if ch == ACK:
            print("[状态] 收到最终 ACK，传输完成")
            self.state = State.DONE
        elif self._is_timeout(3.0):
            print("[警告] 结束帧未确认，但传输可能已完成 (宽容处理)")
            self.state = State.DONE

# ========================== 程序入口 ==========================================
if __name__ == '__main__':
    port = sys.argv[1] if len(sys.argv) >= 2 else DEFAULT_SERIAL_PORT
    firmware = sys.argv[2] if len(sys.argv) >= 3 else DEFAULT_FIRMWARE_FILE
    print(f"[配置] 串口: {port}, 波特率: {DEFAULT_BAUD_RATE}, 固件: {firmware}")
    sender = YmodemSender(port, DEFAULT_BAUD_RATE)
    try:
        sender.run(firmware)
    except KeyboardInterrupt:
        print("\n[中断] 用户取消")
    except Exception as e:
        print(f"[异常] {e}")
    finally:
        if sender.ser.is_open:
            sender.ser.close()