#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""D00 配套命令行终端（与固件 cmd_transport_t 对称的可插拔传输）。

用法：
    python d00term.py                     # 交互：选择传输后进入终端
    python d00term.py com9                # UART 默认 115200
    python d00term.py com9 115200
    python d00term.py tcp 192.168.1.10    # ETH 默认端口 9000
    python d00term.py tcp 192.168.1.10 9000
    python d00term.py com9 -x "ver"       # 单次执行（脚本化/自测）
    python d00term.py --list              # 列出可用 COM 口
    python d00term.py --selftest          # 传输层自检（无需硬件）

会话模式：
    UART：原始透传（设备 shell 自带回显/历史/补全，按键原样转发）
    TCP ：行编辑会话（本地回显 + 上下键历史，设备持有提示符）
    CAN ：扩展点（与固件 CMD_ENABLE_CAN 对应，接入后补一个类 + 一行注册）
"""

import argparse
import os
import socket
import sys
import threading
import time

try:
    import serial
except ImportError:  # pragma: no cover
    serial = None

DEFAULT_TCP_PORT = 9000
DEFAULT_UART_BAUD = 115200
READ_TIMEOUT_S = 0.1


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
    """CAN 扩展点（对应固件 cmd_can.c / CMD_ENABLE_CAN）。
    接入 CAN 硬件后实现 open/send/recv 即可，会话与命令零改动。"""
    name = "CAN"
    mask = 4

    def open(self):
        raise RuntimeError("CAN 传输尚未接入（对应固件 CMD_ENABLE_CAN=0）")

    def send(self, data: bytes):
        raise RuntimeError("CAN 传输尚未接入")

    def recv(self) -> bytes:
        return b""


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


def pick_transport():
    ports = list_ports()
    dev_ip = detect_device_ip()
    _write("D00 命令行终端\r\n")
    _write("  1) UART  " + (", ".join(ports) if ports else "(未检测到 COM 口)") + "\r\n")
    _write(f"  2) ETH   {dev_ip}:9000（自动探测电脑同网段）\r\n")
    _write("  3) CAN   (未接入)\r\n")
    choice = input("选择传输 [1/2/3]: ").strip()
    if choice == "2":
        host = input(f"ETH IP [{dev_ip}]: ").strip() or dev_ip
        return TcpTransport(host, DEFAULT_TCP_PORT)
    if choice == "3":
        return CanTransport()
    if ports:
        default = "COM9" if "COM9" in ports else ports[0]
        hint = "、".join(ports)
        port = input(f"选择串口 [{default}]（可用: {hint}）: ").strip() or default
    else:
        port = input("选择串口 [COM9]: ").strip() or "COM9"
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
        t = cls("COM9") if cls is UartTransport else (
            TcpTransport("192.168.1.10") if cls is TcpTransport else CanTransport())
        print(f"  [OK] {t.name} mask={t.mask} 构造")
    try:
        CanTransport().open()
        ok = False
        print("  [FAIL] CAN 应拒绝 open")
    except RuntimeError:
        print("  [OK] CAN 未接入时正确拒绝")
    if serial is None:
        ok = False
        print("  [WARN] pyserial 未安装（UART 不可用）")
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="d00term", description="D00 配套命令行终端（UART/ETH，CAN 扩展）")
    ap.add_argument("transport", nargs="?", help="com9 或 tcp")
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
        port = args.target or "COM9"
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
