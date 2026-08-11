"""传输层抽象：UART / TCP / HTTP(板端拉取) 三种 OTA 通道。

UART   —— HOSTLINK 帧协议（AA 55 cmd len payload crc16），逐帧确认；
TCP    —— :9020 帧协议（5A cmd len2BE payload crc8），支持流水线；
HTTP   —— 本机起 HTTP 服务提供固件包，控制通道下发 `ota http` 拉取命令。
"""

from __future__ import annotations

import os
import socket
import struct
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial
import serial.tools.list_ports

from .hostlink import crc16


class TransportError(Exception):
    """传输层异常：连接失败/超时/协议错误统一抛此异常。"""


# ================================================================
# UART（HOSTLINK）
# ================================================================


class UartTransport:
    """HOSTLINK 串口通道：负责端口生命周期与逐帧收发。"""

    def __init__(self, port: str, baudrate: int = 921600, timeout: float = 0.3):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self._ser = None

    @staticmethod
    def list_ports() -> list:
        return [p.device for p in serial.tools.list_ports.comports()]

    def open(self):
        try:
            self._ser = serial.Serial(self.port, self.baudrate,
                                      timeout=self.timeout)
        except serial.SerialException as e:
            raise TransportError(f"串口打开失败 {self.port}: {e}") from e

    def close(self):
        if self._ser and self._ser.is_open:
            try:
                self._ser.close()
            except Exception:
                pass
        self._ser = None

    @property
    def is_open(self) -> bool:
        return bool(self._ser and self._ser.is_open)

    def write(self, data: bytes):
        if not self.is_open:
            raise TransportError("串口未打开")
        self._ser.write(data)

    def read(self, size: int = 1) -> bytes:
        if not self.is_open:
            return b""
        return self._ser.read(size)

    def drain(self):
        """丢弃输入缓冲中的残留数据（会话开始前重同步）。"""
        if self.is_open:
            self._ser.reset_input_buffer()

    def cmd(self, frame: bytes, expect_cmd: int, timeout: float = 1.0,
            retries: int = 4, boot_cb=None):
        """发送一帧并等待期望命令的响应；解析器自带重同步。

        :param frame:     待发送帧（build_frame 产物）
        :param expect_cmd: 期望的响应命令码
        :param timeout:    单次轮询超时（秒）
        :param retries:    重试次数
        :param boot_cb:    BOOT 状态广播(0x0C)回调，收到即调用
        :return:           响应帧 bytes；超时返回 None
        """
        from .hostlink import FrameParser, CMD_OTA_BOOT_STATUS
        import time

        parser = FrameParser()
        self.write(frame)
        for _ in range(retries):
            deadline = time.time() + timeout
            while time.time() < deadline:
                n = self._ser.in_waiting if self.is_open else 0
                if n:
                    d = self._ser.read(n)
                    if d:
                        parser.feed(d)
                        for fr in parser.frames():
                            if fr[2] == CMD_OTA_BOOT_STATUS and boot_cb:
                                boot_cb(fr)
                                continue
                            if fr[2] == expect_cmd:
                                return fr
                time.sleep(0.001)
        return None


# ================================================================
# TCP（:9020 OTA 服务器）
# ================================================================


class TcpTransport:
    """:9020 OTA TCP 通道：5A|cmd|len2BE|payload|crc8 帧协议。"""

    MAGIC = 0x5A
    ACK = 0x80

    def __init__(self, ip: str, port: int = 9020, timeout: float = 5.0):
        self.ip = ip
        self.port = port
        self.timeout = timeout
        self._sock = None
        self._rx_buf = bytearray()

    def open(self):
        try:
            self._sock = socket.create_connection((self.ip, self.port),
                                                  timeout=self.timeout)
            self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self._rx_buf.clear()
        except OSError as e:
            raise TransportError(f"TCP 连接失败 {self.ip}:{self.port}: {e}") from e

    def close(self):
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
        self._sock = None

    @property
    def is_open(self) -> bool:
        return bool(self._sock)

    @staticmethod
    def _crc8(data: bytes) -> int:
        crc = 0
        for b in data:
            crc ^= b
            for _ in range(8):
                crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
        return crc

    def send(self, cmd: int, payload: bytes = b""):
        """发送一帧：整帧单次写出，避免 Nagle 拖慢（与固件端一致）。"""
        if not self._sock:
            raise TransportError("TCP 未连接")
        hdr = bytes([self.MAGIC, cmd, (len(payload) >> 8) & 0xFF,
                     len(payload) & 0xFF])
        frame = hdr + payload + bytes([self._crc8(hdr[1:] + payload)])
        self._sock.sendall(frame)

    def _recv_exact(self, n: int, timeout: float) -> bytes:
        self._sock.settimeout(timeout)
        while len(self._rx_buf) < n:
            try:
                chunk = self._sock.recv(n - len(self._rx_buf))
            except socket.timeout:
                raise TransportError("TCP 响应超时") from None
            except OSError as e:
                raise TransportError(f"TCP 连接中断: {e}") from None
            if not chunk:
                raise TransportError("TCP 连接已关闭")
            self._rx_buf += chunk
        out = bytes(self._rx_buf[:n])
        del self._rx_buf[:n]
        return out

    def recv_ack(self, timeout: float = 5.0) -> bytes:
        """读取一帧 ACK，校验 CRC；返回 ACK 载荷。"""
        hdr = self._recv_exact(4, timeout)
        if hdr[0] != self.MAGIC or hdr[1] != self.ACK:
            raise TransportError(f"非法 ACK 帧头: {hdr.hex()}")
        plen = (hdr[2] << 8) | hdr[3]
        body = self._recv_exact(plen + 1, timeout)
        if body[-1] != self._crc8(hdr[1:] + body[:-1]):
            raise TransportError("ACK CRC 校验失败")
        return body[:-1]

    def cmd(self, cmd: int, payload: bytes = b"", timeout: float = 5.0) -> bytes:
        """发送命令并等待 ACK，返回 ACK 载荷。"""
        self.send(cmd, payload)
        return self.recv_ack(timeout)


# ================================================================
# HTTP（板端拉取：本机服务 + 控制通道下发命令）
# ================================================================


class _PkgHandler(BaseHTTPRequestHandler):
    """单文件固件包服务：固定路径 /ota.bin，正确 Content-Length。"""

    pkg_path = ""

    def do_GET(self):  # noqa: N802
        if self.path.split("?")[0] != "/ota.bin":
            self.send_error(404)
            return
        try:
            with open(self.pkg_path, "rb") as f:
                data = f.read()
        except OSError:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):  # 静默访问日志，避免刷屏
        pass


class HttpOtaServer:
    """板端拉取用 HTTP 服务：提供 /ota.bin 固件包。"""

    def __init__(self, pkg_path: str, port: int = 8081):
        self.pkg_path = pkg_path
        self.port = port
        self._srv = None
        self._thread = None

    def start(self, board_ip: str = "") -> str:
        _PkgHandler.pkg_path = self.pkg_path
        try:
            self._srv = ThreadingHTTPServer(("0.0.0.0", self.port),
                                            _PkgHandler)
        except OSError as e:
            raise TransportError(f"HTTP 服务启动失败: {e}") from e
        self._thread = threading.Thread(target=self._srv.serve_forever,
                                        daemon=True)
        self._thread.start()
        return self.url(board_ip)

    def url(self, board_ip: str = "") -> str:
        return f"http://{get_lan_ip(board_ip)}:{self.port}/ota.bin"

    def stop(self):
        if self._srv:
            self._srv.shutdown()
            self._srv.server_close()
            self._srv = None


def get_lan_ip(peer_ip: str = "") -> str:
    """获取能到达 peer_ip 的本地网卡 IP（UDP connect 仅选路不发包）。"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect((peer_ip or "8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"
