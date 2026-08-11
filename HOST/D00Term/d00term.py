#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""D00 配套命令行终端（与固件 cmd_transport_t 对称的可插拔传输）。

用法：
    python d00term.py                     # 交互：选择传输后进入终端
    python d00term.py com5                # UART 默认 115200（缺省自动探测调试口）
    python d00term.py com5 115200
    python d00term.py tcp 192.168.1.10    # ETH 默认端口 9000
    python d00term.py tcp 192.168.1.10 9000
    python d00term.py can                 # CAN（PCAN-USB，需 PEAK 驱动，直调 PCANBasic.dll）
    python d00term.py com5 -x "ver"       # 单次执行（脚本化/自测）
    python d00term.py --list              # 列出可用 COM 口
    python d00term.py --selftest          # 传输层自检（无需硬件）

会话模式：
    UART：原始透传（设备 shell 自带回显/历史/补全，按键原样转发）
    TCP ：行编辑会话（本地回显 + 上下键历史，设备持有提示符）
    CAN ：行帧会话（ID 0x100 下发 / 0x101 回包，首字节序号+0x80 末帧标志，
          与固件 cmd_can.c 的 CMD_ENABLE_CAN 适配器约定一致）
"""

import argparse
import ctypes
import os
import socket
import sys
import threading
import time

try:
    import serial
except ImportError:  # pragma: no cover
    serial = None

try:
    import can
except ImportError:  # pragma: no cover
    can = None

DEFAULT_TCP_PORT = 9000
DEFAULT_UART_BAUD = 115200
READ_TIMEOUT_S = 0.1

# CAN 行帧协议常量（与固件 cmd_can.c 对齐）
CAN_ID_HOST = 0x100    # 主机 -> 设备：shell 命令行下发
CAN_ID_DEV = 0x101     # 设备 -> 主机：shell 回显/应答
CAN_DLC_MAX = 8        # CAN 标准帧数据场最大 8 字节

# PCANBasic.dll 直调（避免依赖已下架的 pcan-basic 包；PEAK 驱动自带 x64 DLL）
_PCAN_DLL_PATHS = (
    r"C:\Windows\System32\PCANBasic.dll",
    r"C:\Program Files\PEAK-System\PCAN-Basic\x64\PCANBasic.dll",
    r"C:\Program Files (x86)\PEAK-System\PCAN-Basic\x64\PCANBasic.dll",
    r"C:\Program Files\PEAK-System\PCAN-Basic\x86\PCANBasic.dll",
    r"C:\Windows\SysWOW64\PCANBasic.dll",
)
_PCAN_USBBUS1 = 0x0051          # 第一个 PCAN-USB 通道
_PCAN_BAUD_500K = 0x001C        # 500 kbit/s（与固件 bxCAN 默认一致）
_PCAN_TYPE_ISA = 0x0001         # USB 设备统一用此类型（IOPort/Interrupt=0）
_PCAN_MESSAGE_STANDARD = 0x00   # 标准帧
_PCAN_ERROR_OK = 0x0000


def _load_pcan_dll():
    """定位并加载 PCANBasic.dll；未安装 PEAK 驱动时返回 None。"""
    if ctypes is None:
        return None
    for path in _PCAN_DLL_PATHS:
        if os.path.exists(path):
            try:
                return ctypes.WinDLL(path)
            except Exception:
                continue
    return None


class _PcanMsg(ctypes.Structure):
    """PCANBasic TPCANMsg 结构体（标准帧数据场 8 字节）。"""
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("msgtype", ctypes.c_ubyte),
        ("len", ctypes.c_ubyte),
        ("data", ctypes.c_ubyte * 8),
    ]


class _PcanApi:
    """极简 PCANBasic.dll 封装：仅 open/write/read/close 四个接口。"""

    def __init__(self, dll):
        self._dll = dll
        self._dll.CAN_Initialize.argtypes = [ctypes.c_ushort, ctypes.c_ushort,
                                             ctypes.c_ushort, ctypes.c_uint32,
                                             ctypes.c_ushort]
        self._dll.CAN_Initialize.restype = ctypes.c_uint32
        self._dll.CAN_Uninitialize.argtypes = [ctypes.c_ushort]
        self._dll.CAN_Uninitialize.restype = ctypes.c_uint32
        self._dll.CAN_Write.argtypes = [ctypes.c_ushort, ctypes.POINTER(_PcanMsg)]
        self._dll.CAN_Write.restype = ctypes.c_uint32
        self._dll.CAN_Read.argtypes = [ctypes.c_ushort, ctypes.POINTER(_PcanMsg),
                                       ctypes.POINTER(ctypes.c_uint64)]
        self._dll.CAN_Read.restype = ctypes.c_uint32
        self._dll.CAN_GetErrorText.argtypes = [ctypes.c_uint32, ctypes.c_ushort,
                                               ctypes.c_char_p]
        self._dll.CAN_GetErrorText.restype = ctypes.c_uint32

    def initialize(self) -> int:
        return self._dll.CAN_Initialize(_PCAN_USBBUS1, _PCAN_BAUD_500K,
                                        _PCAN_TYPE_ISA, 0, 0)

    def write(self, msg: _PcanMsg) -> int:
        return self._dll.CAN_Write(_PCAN_USBBUS1, ctypes.byref(msg))

    def read(self):
        msg = _PcanMsg()
        stamp = ctypes.c_uint64(0)
        rc = self._dll.CAN_Read(_PCAN_USBBUS1, ctypes.byref(msg),
                                ctypes.byref(stamp))
        if rc != _PCAN_ERROR_OK:
            return None
        return bytes(msg.data[:msg.len])

    def uninitialize(self) -> int:
        return self._dll.CAN_Uninitialize(_PCAN_USBBUS1)

    def error_text(self, rc: int) -> str:
        try:
            buf = ctypes.create_string_buffer(128)
            self._dll.CAN_GetErrorText(rc, 0, buf)
            return buf.value.decode("utf-8", "replace")
        except Exception:
            return hex(rc)


# ============================================================
# 传输抽象（与固件 cmd_transport_t{name,mask,start} 一一对应）
# 新增物理协议：继承 Transport 实现 open/send/recv/close，
# 并在 TRANSPORTS 注册表加一行即可，会话代码零改动。
# ============================================================
class Transport:
    name = "?"
    mask = 0

    def open(self):  # pragma: no cover - abstract
        raise NotImplementedError

    def send(self, data: bytes):  # pragma: no cover - abstract
        raise NotImplementedError

    def recv(self) -> bytes:  # pragma: no cover - abstract
        """阻塞最多 READ_TIMEOUT_S，返回已收字节（可空）。"""
        raise NotImplementedError

    def close(self):
        pass


class UartTransport(Transport):
    name = "UART"
    mask = 1

    def __init__(self, port: str, baud: int = DEFAULT_UART_BAUD):
        self.port = port
        self.baud = baud
        self._s = None

    def open(self):
        if serial is None:
            raise RuntimeError("pyserial 未安装：pip install pyserial")
        self._s = serial.Serial(self.port, self.baud, timeout=READ_TIMEOUT_S)

    def send(self, data: bytes):
        self._s.write(data)

    def recv(self) -> bytes:
        n = self._s.in_waiting
        return self._s.read(n if n else 1)

    def close(self):
        if self._s is not None:
            self._s.close()
            self._s = None


class TcpTransport(Transport):
    name = "TCP"
    mask = 2

    def __init__(self, host: str, port: int = DEFAULT_TCP_PORT):
        self.host = host
        self.port = port
        self._s = None

    def open(self):
        self._s = socket.create_connection((self.host, self.port), timeout=5)
        self._s.settimeout(READ_TIMEOUT_S)

    def send(self, data: bytes):
        self._s.sendall(data)

    def recv(self) -> bytes:
        try:
            return self._s.recv(4096)
        except socket.timeout:
            return b""
        except OSError:
            return b""

    def close(self):
        if self._s is not None:
            try:
                self._s.close()
            except OSError:
                pass
            self._s = None


class CanTransport(Transport):
    """CAN 行帧传输（对应固件 cmd_can.c / CMD_ENABLE_CAN）。

    物理层走 PEAK PCAN-USB：优先直调 PCANBasic.dll（ctypes），无 python-can
    依赖；若装了 python-can 也可作为备选后端。帧协议：
      - 下发：ID 0x100，每帧 data[0]=序号（0x80 置位=末帧），data[1..] 为行切片（≤7B）；
      - 回包：ID 0x101 同构，收齐末帧后拼出整行交给会话层。
    未装 PEAK 驱动时 open 抛出可读的 RuntimeError。
    """
    name = "CAN"
    mask = 4

    def __init__(self, channel: str = "PCAN_USBBUS1", bitrate: int = 500000):
        self.channel = channel
        self.bitrate = bitrate
        self._bus = None
        self._api = None
        self._rx_buf = bytearray()   # 当前未拼完的行
        self._rx_seq = 0             # 期望的下一帧序号

    def open(self):
        dll = _load_pcan_dll()
        if dll is not None:
            self._api = _PcanApi(dll)
            rc = self._api.initialize()
            if rc != _PCAN_ERROR_OK:
                raise RuntimeError(f"PCAN-USB 初始化失败: {self._api.error_text(rc)}")
            return
        if can is not None:
            try:
                self._bus = can.interface.Bus(interface="pcan", channel=self.channel,
                                              bitrate=self.bitrate)
                return
            except Exception as e:
                raise RuntimeError(f"PCAN-USB 打开失败: {e}") from e
        raise RuntimeError("PEAK PCAN-USB 驱动未安装（PCANBasic.dll 缺失）；"
                           "请先运行 PEAK-Drivers 安装包")

    def send(self, data: bytes):
        if self._api is None and self._bus is None:
            raise RuntimeError("CAN 未打开")
        for frame in encode_can_line(data):
            if self._api is not None:
                msg = _PcanMsg()
                msg.id = CAN_ID_HOST
                msg.msgtype = _PCAN_MESSAGE_STANDARD
                msg.len = len(frame)
                for i, b in enumerate(frame):
                    msg.data[i] = b
                rc = self._api.write(msg)
                if rc != _PCAN_ERROR_OK:
                    raise RuntimeError(f"PCAN 发送失败: {self._api.error_text(rc)}")
            else:
                self._bus.send(can.Message(arbitration_id=CAN_ID_HOST,
                                           data=frame, is_extended_id=False))

    def recv(self) -> bytes:
        if self._api is None and self._bus is None:
            return b""
        deadline = time.time() + READ_TIMEOUT_S
        while time.time() < deadline:
            if self._api is not None:
                data = self._api.read()
                if data is None:
                    continue
            else:
                msg = self._bus.recv(timeout=0.02)
                if msg is None or msg.arbitration_id != CAN_ID_DEV or not msg.data:
                    continue
                data = msg.data
            seq = data[0] & 0x7F
            last = bool(data[0] & 0x80)
            if seq == 0:
                self._rx_buf = bytearray()
                self._rx_seq = 0
            if seq == self._rx_seq:        # 顺序校验：乱序帧丢弃，防串行
                self._rx_buf += data[1:]
                self._rx_seq += 1
            if last:
                line = bytes(self._rx_buf)
                self._rx_buf = bytearray()
                return line
        return b""

    def close(self):
        if self._api is not None:
            try:
                self._api.uninitialize()
            except Exception:
                pass
            self._api = None
        if self._bus is not None:
            try:
                self._bus.shutdown()
            except Exception:
                pass
            self._bus = None


def encode_can_line(line: bytes):
    """shell 行 -> CAN 帧序列：每帧 ≤7B 负载，首字节为序号（0x80=末帧）。"""
    if not line:
        line = b"\r"
    frames = []
    seq = 0
    for i in range(0, len(line), CAN_DLC_MAX - 1):
        chunk = line[i:i + CAN_DLC_MAX - 1]
        last = (i + CAN_DLC_MAX - 1) >= len(line)
        frames.append(bytes([seq | (0x80 if last else 0)]) + chunk)
        seq = (seq + 1) & 0x7F
    return frames


TRANSPORTS = [UartTransport, TcpTransport, CanTransport]


# ============================================================
# ETH 默认地址：自动探测"与电脑同网段"的设备 IP
# ============================================================
VIRTUAL_SUBNETS = ("192.168.56.", "192.168.119.", "192.168.137.", "169.254.")


def _pc_subnets():
    """枚举电脑物理网卡网段（过滤 VMware/ICS/APIPA/回环）。"""
    subs = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None):
            if info[0] != socket.AF_INET:
                continue
            ip = info[4][0]
            if ip.startswith("127.") or ip.startswith(VIRTUAL_SUBNETS):
                continue
            sub = ".".join(ip.split(".")[:3])
            if sub not in subs:
                subs.append(sub)
    except Exception:
        pass
    return subs


def _tcp_probe(host, port=DEFAULT_TCP_PORT, timeout=0.35):
    try:
        s = socket.create_connection((host, port), timeout=timeout)
        s.close()
        return True
    except OSError:
        return False


def detect_device_ip():
    """ETH 默认设备 IP：
    1) 出厂 IP 192.168.1.10 可达 → 用它；
    2) 依次探测电脑各网段的 .10（可达即命中）；
    3) 兜底：第一个电脑网段的 .10（提示用户用 net ip 设置一次即可持久化）。"""
    if _tcp_probe("192.168.1.10"):
        return "192.168.1.10"
    for sub in _pc_subnets():
        ip = f"{sub}.10"
        if _tcp_probe(ip):
            return ip
    subs = _pc_subnets()
    return f"{subs[0]}.10" if subs else "192.168.1.10"


# ============================================================
# 交互会话
# ============================================================
def _key_available():
    if os.name == "nt":
        import msvcrt
        return msvcrt.kbhit()
    import select
    return select.select([sys.stdin], [], [], 0)[0]


def _read_key():
    if os.name == "nt":
        import msvcrt
        return msvcrt.getch()
    return sys.stdin.buffer.read(1)


def _write(text: str):
    sys.stdout.write(text)
    sys.stdout.flush()


def run_uart_raw(t: UartTransport):
    """原始透传：设备 shell 自带回显/历史/补全，按键原样转发。"""
    _write(f"[D00Term] UART {t.port}@{t.baud} 连接成功，Ctrl+C 退出\r\n")
    try:
        while True:
            if _key_available():
                ch = _read_key()
                if ch in (b"\x03",):            # Ctrl+C
                    break
                t.send(ch)
            data = t.recv()
            if data:
                try:
                    _write(data.decode("utf-8", "replace"))
                except Exception:
                    pass
    except KeyboardInterrupt:
        pass


def run_tcp_line(t: TcpTransport):
    """行编辑会话：本地回显 + 上下键历史，设备持有提示符。"""
    history = []
    hist_pos = -1
    line = bytearray()
    _write(f"[D00Term] TCP {t.host}:{t.port} 连接成功，Ctrl+C 退出\r\n")

    def reader():
        while True:
            data = t.recv()
            if not data:
                return
            try:
                _write(data.decode("utf-8", "replace"))
            except Exception:
                pass

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    try:
        while True:
            if not _key_available():
                time.sleep(0.02)
                continue
            ch = _read_key()
            if ch in (b"\x03",):                # Ctrl+C
                break
            if ch in (b"\r", b"\n"):
                cmd = bytes(line)
                if cmd:
                    history.append(cmd.decode("utf-8", "replace"))
                    if len(history) > 50:
                        history.pop(0)
                    t.send(cmd + b"\r\n")
                line.clear()
                hist_pos = -1
            elif ch == b"\x08" or ch == b"\x7f":   # Backspace
                if line:
                    line.pop()
                    _write("\b \b")
            elif ch == b"\xe0" or ch == b"\x00":   # 扩展键（方向键）
                if _key_available():
                    ch2 = _read_key()
                    if ch2 == b"H" and history:     # Up
                        hist_pos = max(0, hist_pos - 1) if hist_pos >= 0 else len(history) - 1
                        _draw_line(line, history[hist_pos])
                        line = bytearray(history[hist_pos].encode("utf-8"))
                    elif ch2 == b"P" and history:   # Down
                        hist_pos += 1
                        if hist_pos >= len(history):
                            hist_pos = -1
                            _draw_line(line, "")
                            line.clear()
                        else:
                            _draw_line(line, history[hist_pos])
                            line = bytearray(history[hist_pos].encode("utf-8"))
            elif 32 <= ch[0] <= 126:
                line.append(ch[0])
                _write(ch.decode("ascii", "replace"))
    except KeyboardInterrupt:
        pass
    _write("\r\n[D00Term] 已退出\r\n")


def _draw_line(old: bytearray, new: str):
    back = "\b" * len(old)
    pad = " " * max(0, len(old) - len(new))
    _write(back + new + pad + "\b" * max(0, len(pad)))


# ============================================================
# 传输选择 / 单次执行 / 自检
# ============================================================
def list_ports():
    if serial is None:
        return []
    ports = []
    try:
        ports = [p.device for p in serial.tools.list_ports.comports()]
    except Exception:
        ports = []
    if not ports and os.name == "nt":
        # 兜底：直接读注册表 SERIALCOMM（不依赖 WMI/枚举服务）
        try:
            import winreg
            with winreg.OpenKey(
                    winreg.HKEY_LOCAL_MACHINE,
                    r"HARDWARE\DEVICEMAP\SERIALCOMM") as key:
                i = 0
                while True:
                    try:
                        ports.append(winreg.EnumValue(key, i)[1])
                        i += 1
                    except OSError:
                        break
        except Exception:
            pass
    return ports


def default_uart_port():
    """默认调试串口：优先 CH340/CH9102（探索者板载），其次 COM5，再其次任意可用口。"""
    ports = list_ports()
    if not ports:
        return "COM5"
    try:
        for p in serial.tools.list_ports.comports():
            desc = (p.description or "").upper()
            if "CH340" in desc or "CH9102" in desc:
                return p.device
    except Exception:
        pass
    return "COM5" if "COM5" in ports else ports[0]


def pick_transport():
    ports = list_ports()
    dev_ip = detect_device_ip()
    _write("D00 命令行终端\r\n")
    _write("  1) UART  " + (", ".join(ports) if ports else "(未检测到 COM 口)") + "\r\n")
    _write(f"  2) ETH   {dev_ip}:9000（自动探测电脑同网段）\r\n")
    _write("  3) CAN   " + ("PCAN-USB" if _load_pcan_dll() is not None
                            else ("python-can" if can is not None
                                  else "(未装 PEAK 驱动)")) + "\r\n")
    choice = input("选择传输 [1/2/3]: ").strip()
    if choice == "2":
        host = input(f"ETH IP [{dev_ip}]: ").strip() or dev_ip
        return TcpTransport(host, DEFAULT_TCP_PORT)
    if choice == "3":
        return CanTransport()
    if ports:
        default = default_uart_port()
        hint = "、".join(ports)
        port = input(f"选择串口 [{default}]（可用: {hint}）: ").strip() or default
    else:
        port = input("选择串口 [COM5]: ").strip() or "COM5"
    return UartTransport(port, DEFAULT_UART_BAUD)


def run_once(t, cmd: str, wait_s: float = 1.2):
    """单次执行：发一条命令，采集应答后退出（调用方负责 open/close）。"""
    if isinstance(t, UartTransport):
        t.send(cmd.encode("utf-8", "replace") + b"\r")
    else:
        t.send(cmd.encode("utf-8", "replace") + b"\r\n")
    end = time.time() + wait_s
    out = b""
    while time.time() < end:
        data = t.recv()
        if data:
            out += data
    sys.stdout.buffer.write(out)
    sys.stdout.flush()
    return 0


def selftest():
    """传输层自检（无需硬件）：实例化/参数/异常路径。"""
    ok = True
    for cls in (UartTransport, TcpTransport, CanTransport):
        t = cls("COM5") if cls is UartTransport else (
            TcpTransport("192.168.1.10") if cls is TcpTransport else CanTransport())
        print(f"  [OK] {t.name} mask={t.mask} 构造")
    if _load_pcan_dll() is None and can is None:
        try:
            CanTransport().open()
            ok = False
            print("  [FAIL] CAN 应拒绝 open（未装 PEAK 驱动）")
        except RuntimeError:
            print("  [OK] CAN 未接入时正确拒绝")
    else:
        print("  [OK] CAN 传输就绪（PEAK 驱动/python-can 已装，硬件在线与否由 open 判定）")
    if serial is None:
        ok = False
        print("  [WARN] pyserial 未安装（UART 不可用）")
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="d00term", description="D00 配套命令行终端（UART/ETH/CAN）")
    ap.add_argument("transport", nargs="?", help="uart/com / tcp / can（缺省交互选择）")
    ap.add_argument("target", nargs="?", help="串口号或 ETH IP")
    ap.add_argument("port_or_baud", nargs="?", help="TCP 端口或 UART 波特率")
    ap.add_argument("-x", "--exec", metavar="CMD", help="单次执行一条命令后退出")
    ap.add_argument("--list", action="store_true", help="列出可用 COM 口")
    ap.add_argument("--selftest", action="store_true", help="传输层自检")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if args.list:
        print("\n".join(list_ports()) if list_ports() else "未检测到 COM 口")
        return 0

    tl = (args.transport or "").lower()
    if args.transport is None:
        t = pick_transport()
    elif tl.startswith("com") or tl.startswith("\\\\.\\"):
        port = args.transport
        baud = int(args.target) if args.target else DEFAULT_UART_BAUD
        t = UartTransport(port, baud)
    elif tl in ("uart", "com"):
        port = args.target or default_uart_port()
        baud = int(args.port_or_baud) if args.port_or_baud else DEFAULT_UART_BAUD
        t = UartTransport(port, baud)
    elif tl == "tcp":
        host = args.target or detect_device_ip()
        port = int(args.port_or_baud) if args.port_or_baud else DEFAULT_TCP_PORT
        t = TcpTransport(host, port)
    elif tl == "can":
        t = CanTransport()
    else:
        ap.error(f"未知传输: {args.transport}（支持 uart/com / tcp / can）")

    try:
        t.open()
    except Exception as e:
        if isinstance(t, UartTransport):
            avail = "、".join(list_ports()) or "无"
            print(f"[D00Term] 连接失败: {e}\n         可用串口: {avail}")
        else:
            print(f"[D00Term] 连接失败: {e}")
        return 1

    try:
        if args.exec is not None:
            try:
                return run_once(t, args.exec)
            except Exception as e:
                print(f"[D00Term] 执行失败: {e}")
                return 1
        if isinstance(t, UartTransport):
            run_uart_raw(t)
        else:
            run_tcp_line(t)
    finally:
        t.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
