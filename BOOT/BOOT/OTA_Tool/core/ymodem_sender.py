#!/usr/bin/env python3
"""
===============================================================================
 文件名称: ymodem_sender.py
 描述:     工业级 Ymodem 安全固件发送器 (QT 上位机集成版)
           - 基于已验证的命令行脚本，仅增加回调支持，核心逻辑不变
===============================================================================
"""

import time
import struct
import os
import serial
from Crypto.Cipher import AES
from Crypto.Hash import SHA256
from ecdsa import SigningKey, NIST256p

# ========================== 协议常量 ==========================
SOH = 0x01
STX = 0x02
EOT = 0x04
ACK = 0x06
NAK = 0x15
CAN = 0x18
C   = 0x43

PACKET_SIZE      = 1024
FILE_INFO_SIZE   = 128
MAX_RETRY        = 5
INTER_BYTE_DELAY = 0.005

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

# ========================== CRC32 工具函数 ==========================
def crc32(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ byte) & 0xFF]
    return crc ^ 0xFFFFFFFF

# ========================== UID 密钥派生 ==========================
def derive_aes_key_from_uid(uid_hex: str) -> bytes:
    uid_ints = [int(uid_hex[i:i+8], 16) for i in range(0, 24, 8)]
    uid_bytes = struct.pack('<III', *uid_ints)
    salt = b"OTA-AES-KEY-V1\x00"
    h = SHA256.new(uid_bytes + salt)
    return h.digest()

# ========================== AES-256-CTR 加密 ==========================
def aes_ctr_encrypt(key: bytes, iv12: bytes, plain: bytes) -> bytes:
    iv16 = iv12 + b'\x00' * 4
    cipher = AES.new(key, AES.MODE_ECB)
    encrypted = bytearray()
    counter = bytearray(iv16)
    for i in range(0, len(plain), 16):
        block = plain[i:i+16]
        keystream = cipher.encrypt(bytes(counter))
        for j in range(len(block)):
            encrypted.append(block[j] ^ keystream[j])
        carry = 1
        for k in range(15, -1, -1):
            carry, counter[k] = divmod(counter[k] + carry, 256)
    return bytes(encrypted)

# ========================== 加密 + 签名 ==========================
def encrypt_and_sign(input_bin: str, output_bin: str, private_key_hex: str,
                     aes_key_hex: str, version: int = 1):
    with open(input_bin, 'rb') as f:
        plain = f.read()

    aes_key = bytes.fromhex(aes_key_hex)
    iv = os.urandom(12)
    encrypted = aes_ctr_encrypt(aes_key, iv, plain)

    magic = 0x4F5441FE
    header = struct.pack('<III12s8s', magic, version, len(plain), iv, b'\x00' * 8)

    h = SHA256.new(header + encrypted)
    digest = h.digest()
    sk = SigningKey.from_string(bytes.fromhex(private_key_hex), curve=NIST256p)
    signature = sk.sign_digest(digest)

    with open(output_bin, 'wb') as f:
        f.write(header + encrypted + signature)

# ========================== 状态机 ==========================
class State:
    WAIT_C          = 0
    SEND_FILE_INFO  = 1
    WAIT_ACK_C      = 2
    SEND_DATA       = 3
    SEND_EOT_FIRST  = 4
    WAIT_NAK        = 5
    SEND_EOT_SECOND = 6
    WAIT_ACK_C2     = 7
    SEND_END_FRAME  = 8
    WAIT_ACK_END    = 9
    DONE            = 10
    ERROR           = 11

class YmodemSender:
    """ Ymodem 发送器，支持日志和进度回调 """

    def __init__(self, port=None, baudrate=115200, timeout=0.5,
                 log_callback=None, progress_callback=None, serial_instance=None):
        if serial_instance:
            self.ser = serial_instance
            self._own_serial = False
        else:
            self.ser = serial.Serial(port, baudrate, timeout=timeout)
            self._own_serial = True

        self.state = State.WAIT_C
        self.retry = 0
        self.seq = 0
        self.data = b''
        self.file_name = ''
        self.file_size = 0
        self.file_crc = 0
        self.offset = 0
        self.last_offset = 0
        self.timer = 0
        self._data_mode = False

        self.log = log_callback if log_callback else lambda msg, color=None: print(msg)
        self.report_progress = progress_callback if progress_callback else lambda p: None

    def _set_timeout(self, seconds: float):
        self.timer = time.time()

    def _is_timeout(self, seconds: float) -> bool:
        return (time.time() - self.timer) >= seconds

    def _read_byte(self, timeout_s: float = 0.5) -> int:
        self.ser.timeout = timeout_s
        byte = self.ser.read(1)
        return byte[0] if byte else -1

    def _send_packet(self, packet: bytes):
        self.ser.write(packet)
        self.ser.flush()
        time.sleep(INTER_BYTE_DELAY)

    def _build_file_info_frame(self) -> bytes:
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
        assert len(chunk) == PACKET_SIZE
        seq_comp = 0xFF ^ seq
        header = struct.pack('>BBB', STX, seq, seq_comp)
        crc_val = crc32(chunk)
        crc_bytes = struct.pack('<I', crc_val)
        return header + chunk + crc_bytes

    def _build_end_frame(self) -> bytes:
        header = struct.pack('>BBB', SOH, 0x00, 0xFF)
        zero_data = b'\x00' * FILE_INFO_SIZE
        crc_val = crc32(zero_data)
        crc_bytes = struct.pack('<I', crc_val)
        return header + zero_data + crc_bytes

    def send_file(self, file_path: str) -> bool:
        if not os.path.exists(file_path):
            self.log(f"[错误] 文件不存在: {file_path}", "red")
            return False
        with open(file_path, 'rb') as f:
            self.data = f.read()
        self.file_size = len(self.data)
        self.file_name = os.path.basename(file_path)
        self.file_crc = crc32(self.data)
        self.log(f"[信息] 文件: {self.file_name}, 大小: {self.file_size}, CRC32: 0x{self.file_crc:08X}", "cyan")

        # 重置状态机
        self.state = State.WAIT_C
        self.retry = 0
        self.seq = 1
        self.offset = 0
        self.last_offset = 0
        self._data_mode = False
        self._set_timeout(10.0)

        while self.state not in [State.DONE, State.ERROR]:
            self._dispatch()

        if self._own_serial:
            self.ser.close()
            
        if self.state == State.DONE:
            self.log("[成功] 固件升级文件发送完毕", "green")
            return True
        else:
            self.log("[失败] 传输中止", "red")
            return False

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

    def _handle_wait_c(self):
        ch = self._read_byte(0.2)
        if ch == C:
            self.log("收到握手 'C'，开始发送文件信息", "green")
            self.state = State.SEND_FILE_INFO
            self.retry = 0
        if self._is_timeout(10.0):
            self.log("等待 'C' 超时", "red")
            self.state = State.ERROR

    def _handle_send_file_info(self):
        self.log("发送文件信息帧...", "cyan")
        frame = self._build_file_info_frame()
        self._send_packet(frame)
        self._set_timeout(3.0)
        self.state = State.WAIT_ACK_C
        self.retry = 0
        self._data_mode = False

    def _handle_wait_ack_c(self):
        ch1 = self._read_byte(0.5)
        if ch1 < 0:
            pass
        elif ch1 == ACK:
            if not self._data_mode:
                ch2 = self._read_byte(0.5)
                if ch2 == C:
                    self.log("收到 ACK + 'C'，开始传输数据", "green")
                    self.state = State.SEND_DATA
                    self.retry = 0
                    self.seq = 1
                    self.offset = 0
                    return
                else:
                    if ch2 >= 0:
                        self.log(f"预期 'C' 但收到 0x{ch2:02X}", "orange")
            else:
                self.log(f"数据帧 #{self.seq-1} 确认", "green")
                self.seq = (self.seq + 1) & 0xFF
                if self.seq == 0:
                    self.seq = 1
                self.offset = self.last_offset + PACKET_SIZE
                # 进度更新
                progress = int((self.offset / self.file_size) * 100)
                self.report_progress(min(progress, 100))
                self.state = State.SEND_DATA
                self.retry = 0
                return
        elif ch1 == NAK:
            self.log("收到 NAK，准备重传", "orange")
            if self.retry < MAX_RETRY:
                self.retry += 1
                if self._data_mode:
                    self.offset = self.last_offset
                self.state = State.SEND_DATA if self._data_mode else State.SEND_FILE_INFO
            else:
                self.log("重试次数耗尽", "red")
                self.state = State.ERROR
            return

        if self._is_timeout(5.0):
            if self.retry < MAX_RETRY:
                self.log(f"超时重试 ({self.retry+1}/{MAX_RETRY})", "orange")
                self.retry += 1
                if self._data_mode:
                    self.offset = self.last_offset
                self.state = State.SEND_DATA if self._data_mode else State.SEND_FILE_INFO
            else:
                self.log("无响应超时", "red")
                self.state = State.ERROR

    def _handle_send_data(self):
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
        self.log(f"发送数据帧 #{self.seq}, 偏移 {self.offset}", "cyan")
        self.offset += PACKET_SIZE
        self._set_timeout(3.0)
        self.state = State.WAIT_ACK_C
        self._data_mode = True
        self.retry = 0

    def _handle_send_eot_first(self):
        self.log("发送 EOT (第一次)", "cyan")
        self._send_packet(bytes([EOT]))
        self._set_timeout(2.0)
        self.state = State.WAIT_NAK
        self.retry = 0

    def _handle_wait_nak(self):
        ch = self._read_byte(0.5)
        if ch == NAK:
            self.log("收到 NAK，发送第二次 EOT", "cyan")
            self.state = State.SEND_EOT_SECOND
            self.retry = 0
        elif ch >= 0:
            self.log(f"预期 NAK 但收到 0x{ch:02X}", "orange")
        if self._is_timeout(3.0):
            if self.retry < MAX_RETRY:
                self.log(f"重新发送 EOT ({self.retry+1})", "orange")
                self.retry += 1
                self.state = State.SEND_EOT_FIRST
            else:
                self.log("EOT 阶段超时", "red")
                self.state = State.ERROR

    def _handle_send_eot_second(self):
        self.log("发送 EOT (第二次)", "cyan")
        self._send_packet(bytes([EOT]))
        self._set_timeout(2.0)
        self.state = State.WAIT_ACK_C2
        self.retry = 0

    def _handle_wait_ack_c2(self):
        ch1 = self._read_byte(0.5)
        if ch1 == ACK:
            ch2 = self._read_byte(0.5)
            if ch2 == C:
                self.log("收到 ACK + 'C'，发送结束帧", "green")
                self.state = State.SEND_END_FRAME
                return
        if self._is_timeout(5.0):
            if self.retry < MAX_RETRY:
                self.retry += 1
                self.state = State.SEND_EOT_SECOND
            else:
                self.log("结束握手超时", "red")
                self.state = State.ERROR

    def _handle_send_end_frame(self):
        self.log("发送结束帧", "cyan")
        frame = self._build_end_frame()
        self._send_packet(frame)
        self._set_timeout(3.0)
        self.state = State.WAIT_ACK_END

    def _handle_wait_ack_end(self):
        ch = self._read_byte(0.5)
        if ch == ACK:
            self.log("收到最终 ACK，传输完成", "green")
            self.state = State.DONE
        elif self._is_timeout(3.0):
            self.log("结束帧未确认，但传输可能已完成", "orange")
            self.state = State.DONE