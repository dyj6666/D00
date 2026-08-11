"""OTA 升级引擎：UART / TCP / HTTP 三通道统一驱动。

职责：
  - 固件包构建（加密 + 签名，build 号单调递增）
  - 传输驱动：UART 逐帧确认 / TCP 流水线 / HTTP 板端拉取
  - 断点续传（UART/TCP 查询 STATUS 自动续传）
  - 速率计量、ETA、BOOT 阶段可视化与升级后验证
"""

from __future__ import annotations

import os
import re
import socket
import struct
import time

from PyQt5.QtCore import QThread, pyqtSignal

from .hostlink import (CMD_OTA_BEGIN, CMD_OTA_DATA, CMD_OTA_END,
                       CMD_OTA_STATUS, CMD_OTA_BOOT_STATUS, CMD_OTA_RESET,
                       build_ota_begin, build_ota_data, build_ota_end,
                       build_ota_status, build_ota_reset, parse_ota_status)
from .transport import (HttpOtaServer, TcpTransport, TransportError,
                        UartTransport)
from .ymodem_sender import YmodemSender, encrypt_and_sign, derive_aes_key_from_uid
from .version_lib import add_entry, alloc_build_no, chip_id_int, load_lib


class OtaEngine(QThread):
    """一次 OTA 会话的驱动线程；与界面完全解耦，可无头测试。"""

    log = pyqtSignal(str, str)                 # 消息, 颜色
    progress = pyqtSignal(int, int, float, float)  # bytes, total, speed_bps, eta_s
    stage = pyqtSignal(str)                    # 阶段名（见 STAGE_*）
    device_info = pyqtSignal(dict)             # 设备信息/验证结果
    finished = pyqtSignal(bool, str)           # 成功?, 摘要

    STAGE_IDLE = "IDLE"
    STAGE_ERASING = "ERASING"
    STAGE_DOWNLOADING = "DOWNLOADING"
    STAGE_VERIFYING = "VERIFYING"
    STAGE_COMMITTED = "COMMITTED"
    STAGE_RUNNING = "RUNNING"
    STAGE_DONE = "DONE"
    STAGE_FAIL = "FAIL"

    _BOOT_STAGE = {
        1: "VERIFYING", 2: "VERIFYING", 3: "VERIFYING",
        4: "VERIFYING", 5: "VERIFYING",
        6: "COMMITTED", 7: "RUNNING",
    }
    _BOOT_NAME = {
        1: "探测预下载包", 2: "安全校验", 3: "备份 RUN", 4: "擦除 APP",
        5: "解密写入 RUN", 6: "提交待确认", 7: "完成重启",
    }

    def __init__(self, mode: str, cfg: dict, parent=None):
        super().__init__(parent)
        self.mode = mode                       # uart / tcp / http
        self.cfg = cfg
        self._stop_flag = False
        self._pkg = b""
        self._pkg_path = ""
        self._build_no = int(cfg.get("build_no", 0) or 0)
        self._speed = 0.0                      # 平滑后的瞬时速率
        self._last_t = 0.0
        self._last_b = 0

    # ------------------------------------------------------------------
    # 对外控制
    # ------------------------------------------------------------------
    def stop(self):
        self._stop_flag = True

    # ------------------------------------------------------------------
    # 主流程
    # ------------------------------------------------------------------
    def run(self):
        try:
            self._stage(self.STAGE_IDLE)
            self._log("═" * 52, "cyan")
            self._log(f"== D00 OTA 引擎 · 模式: {self.mode.upper()} ==", "cyan")
            self._prepare_package()
            if self.mode == "uart" and self.cfg.get("use_ymodem"):
                self._run_ymodem()
            elif self.mode == "uart":
                self._run_uart()
            elif self.mode == "tcp":
                self._run_tcp()
            elif self.mode == "http":
                self._run_http()
            else:
                raise ValueError(f"未知模式: {self.mode}")
            if self._stop_flag:
                self._log("用户中止", "orange")
                self.finished.emit(False, "已停止")
                return
            self._stage(self.STAGE_DONE)
            self._log("✅ 升级流程完成", "green")
            self.finished.emit(True, "成功")
        except TransportError as e:
            self._log(f"❌ 传输错误: {e}", "red")
            self._stage(self.STAGE_FAIL)
            self.finished.emit(False, str(e))
        except Exception as e:  # noqa: BLE001
            self._log(f"❌ 异常: {e}", "red")
            self._stage(self.STAGE_FAIL)
            self.finished.emit(False, str(e))

    # ------------------------------------------------------------------
    # YMODEM 传统模式（BOOT 直接升级）
    # ------------------------------------------------------------------
    def _run_ymodem(self):
        self._log("== YMODEM 传统升级（BOOT）==", "cyan")
        port = self.cfg["uart_port"]
        baud = int(self.cfg.get("uart_baud", 115200))
        uart = UartTransport(port, baud)
        uart.open()
        try:
            self._log("请确认设备已进入 BOOT 升级模式（发 ota 命令复位）",
                      "orange")
            sender = YmodemSender(
                serial_instance=uart._ser,
                log_callback=lambda m, c="white": self._log(m, c),
                progress_callback=lambda p: self.progress.emit(
                    int(len(self._pkg) * p / 100), len(self._pkg),
                    int(self._speed), 0))
            ok = sender.send_file(self._pkg_path)
            if ok:
                self._stage(self.STAGE_COMMITTED)
            else:
                raise TransportError("YMODEM 传输失败")
        finally:
            uart.close()

    # ------------------------------------------------------------------
    # 固件包准备
    # ------------------------------------------------------------------
    def _prepare_package(self):
        file_path = self.cfg["file"]
        version = int(self.cfg.get("version", 1))
        uid = self.cfg.get("uid", "")
        key = self.cfg.get("key", "")
        if not all([file_path, uid, key]):
            raise TransportError("固件/UID/私钥 参数缺失")
        lib = load_lib()
        chip = chip_id_int(lib)
        if self._build_no <= 0:
            # 单设备自动分配；批量场景由调用方预分配（避免并发竞争）
            self._build_no = alloc_build_no(lib)
            add_entry(version, self._build_no, file_path, "auto")
        aes_key = derive_aes_key_from_uid(uid)
        self._pkg_path = os.path.join(os.path.dirname(file_path), "_ota_pkg.bin")
        encrypt_and_sign(file_path, self._pkg_path, key, aes_key.hex(),
                         version, chip, self._build_no)
        with open(self._pkg_path, "rb") as f:
            self._pkg = f.read()
        self._log(f"固件: {os.path.basename(file_path)}", "yellow")
        self._log(f"版本 v{version} · 构建 #{self._build_no} · 芯片 0x{chip:X}",
                  "yellow")
        self._log(f"安全包: {len(self._pkg)} 字节（AES-CTR + ECDSA 签名）",
                  "cyan")

    # ------------------------------------------------------------------
    # UART（HOSTLINK）模式
    # ------------------------------------------------------------------
    def _run_uart(self):
        self._log("== UART(HOSTLINK) 运行时 OTA ==", "cyan")
        port = self.cfg["uart_port"]
        baud = int(self.cfg.get("uart_baud", 921600))
        uart = UartTransport(port, baud)
        uart.open()
        try:
            uart.drain()
            # 1) BEGIN：擦除下载区（约 2-4s），放宽超时
            self._stage(self.STAGE_ERASING)
            self._log("发送 BEGIN（擦除下载区）...", "cyan")
            r = uart.cmd(build_ota_begin(int(self.cfg["version"]), len(self._pkg)),
                         CMD_OTA_BEGIN, timeout=10, boot_cb=self._on_boot_frame)
            if r is None or r[5] != 0:
                raise TransportError(f"BEGIN 失败: {r[5] if r else '无响应'}")
            self._stage(self.STAGE_DOWNLOADING)

            # 2) 断点续传：查询 APP 已收进度
            start = 0
            r = uart.cmd(build_ota_status(), CMD_OTA_STATUS, timeout=2)
            if r is not None:
                st, rx, total = parse_ota_status(r[5:])
                if st == 1 and 0 < rx < len(self._pkg):
                    start = rx
                    self._log(f"检测到断点，从 {rx} 字节续传", "yellow")

            # 3) DATA 逐块发送（无固定 sleep，紧贴 ACK 节奏，速率拉满）
            chunk = 240
            for off in range(start, len(self._pkg), chunk):
                if self._stop_flag:
                    return
                blk = self._pkg[off:off + chunk]
                r = uart.cmd(build_ota_data(off, blk), CMD_OTA_DATA, timeout=2)
                if r is None:
                    raise TransportError(f"数据块 {off} 无响应")
                if r[5] != 0:
                    raise TransportError(f"数据块 {off} 状态 {r[5]}")
                rx = struct.unpack("<I", r[6:10])[0]
                self._report(rx, len(self._pkg))
                if off % (48 * 1024) == 0 and off > 0:
                    self._log(f"  已下载 {rx}/{len(self._pkg)} 字节", "gray")

            # 4) STATUS 复核
            r = uart.cmd(build_ota_status(), CMD_OTA_STATUS, timeout=2)
            if r is not None:
                st, rx, total = parse_ota_status(r[5:])
                self._log(f"STATUS: state={st} {rx}/{total}", "cyan")

            # 5) END 触发 BOOT 切换（Ota_End 直接复位，无响应属正常）
            self._log("发送 END，触发 BOOT 切换...", "cyan")
            uart.cmd(build_ota_end(), CMD_OTA_END, timeout=1.5, retries=1)
            self._stage(self.STAGE_VERIFYING)
            self.progress.emit(len(self._pkg), len(self._pkg),
                               int(self._speed), 0)
            # 6) 并发：COM13 听 BOOT 状态广播 + COM9 抓启动日志
            boot_thread = None
            if self.cfg.get("verify_boot_log"):
                import threading
                boot_thread = threading.Thread(
                    target=self._verify_boot_log, daemon=True)
                boot_thread.start()
            self._listen_uart_boot(uart, 15)
            if boot_thread:
                boot_thread.join(timeout=22)
            # 8) 可选：HTTP 状态页验证
            if self.cfg.get("verify_http"):
                self._verify_http_status()
        finally:
            uart.close()

    def _listen_uart_boot(self, uart: UartTransport, duration: float):
        """升级后监听 BOOT 阶段广播，实时可视化校验/备份/写入/提交。"""
        parser = None
        from .hostlink import FrameParser
        parser = FrameParser()
        deadline = time.time() + duration
        while time.time() < deadline and not self._stop_flag:
            n = uart._ser.in_waiting if uart.is_open else 0
            if n:
                d = uart.read(n)
                if d:
                    parser.feed(d)
                    for fr in parser.frames():
                        if fr[2] == CMD_OTA_BOOT_STATUS:
                            self._on_boot_frame(fr)
            time.sleep(0.01)

    def _on_boot_frame(self, frame: bytes):
        from .hostlink import FrameParser
        payload = FrameParser.payload(frame)
        if len(payload) < 2:
            return
        phase, err = payload[0], payload[1]
        if phase == 0xFF:
            self._log(f"BOOT 升级失败 (err={err})", "red")
            self._stage(self.STAGE_FAIL)
            return
        name = self._BOOT_NAME.get(phase, f"未知({phase})")
        st = self._BOOT_STAGE.get(phase)
        if st:
            self._stage(st)
        self._log(f"BOOT: {name}", "cyan")

    # ------------------------------------------------------------------
    # TCP（:9020）模式
    # ------------------------------------------------------------------
    def _run_tcp(self):
        self._log("== TCP(:9020) 运行时 OTA ==", "cyan")
        ip = self.cfg["tcp_ip"]
        port = int(self.cfg.get("tcp_port", 9020))
        tcp = TcpTransport(ip, port)
        tcp.open()
        try:
            # 0) 可选：强制从零开始
            if self.cfg.get("no_resume"):
                tcp.cmd(5, b"", timeout=3)   # RESET
                time.sleep(0.2)
            # 1) BEGIN（擦除 2-4s）
            self._stage(self.STAGE_ERASING)
            self._log("发送 BEGIN（擦除下载区）...", "cyan")
            r = tcp.cmd(1, struct.pack(">II", int(self.cfg["version"]),
                                       len(self._pkg)), timeout=15)
            if r[0] != 0:
                raise TransportError(f"BEGIN 失败: {r[0]}")
            self._stage(self.STAGE_DOWNLOADING)

            # 2) 断点续传
            start = 0
            try:
                r = tcp.cmd(4, b"", timeout=3)   # STATUS
                if r and r[0] == 1:
                    rx = int.from_bytes(r[1:5], "big")
                    if 0 < rx < len(self._pkg):
                        start = rx
                        self._log(f"检测到断点，从 {rx} 字节续传", "yellow")
            except TransportError:
                pass

            # 3) 流水线 DATA：窗口可配，隐藏网络 RTT，逼近 Flash 写入极限
            # 默认 8（2KB 突发）：实测 10+ 次 100% 稳定，96KB/0.5s ≈ 190KB/s，
            # 已逼近 Flash 写入极限；更大窗口偶发 ACK 传输停滞（已定位为
            # 板端小窗口/分段交互，非服务挂死），保留可调
            window = int(self.cfg.get("pipe_window", 8))
            self._log(f"流水线窗口: {window} 块（{window * 240 // 1024}KB）",
                      "gray")
            sent = start
            pending = 0
            while sent < len(self._pkg) or pending > 0:
                if self._stop_flag:
                    return
                while pending < window and sent < len(self._pkg):
                    blk = self._pkg[sent:sent + 240]
                    tcp.send(2, struct.pack(">I", sent) + blk)
                    sent += len(blk)
                    pending += 1
                r = tcp.recv_ack(timeout=5)
                pending -= 1
                if r[0] != 0:
                    raise TransportError(f"数据块失败，状态 {r[0]}")
                rx = int.from_bytes(r[1:5], "big")
                self._report(rx, len(self._pkg))
                if rx % (48 * 1024) < 240 and rx > 0:
                    self._log(f"  已下载 {rx}/{len(self._pkg)} 字节", "gray")

            # 4) END：设备校验后复位，连接随之断开（无响应属正常）
            self._log("发送 END，触发 BOOT 切换...", "cyan")
            try:
                tcp.cmd(3, b"", timeout=1.5)
            except TransportError:
                pass
            self._stage(self.STAGE_VERIFYING)
            self.progress.emit(len(self._pkg), len(self._pkg),
                               int(self._speed), 0)
            # 5) 验证：轮询 :8080 状态页确认重启与新版本
            if self.cfg.get("verify_http"):
                if self.cfg.get("verify_mode", "fast") == "fast":
                    # 快速验证：优先 :9020 STATUS（APP OTA 服务在线=新固件在跑），
                    # 失败才回退 :8080 状态页（严格路径），安全不减
                    if not self._probe_tcp_status(ip):
                        self._verify_http_status(ip)
                else:
                    self._verify_http_status(ip)
        finally:
            tcp.close()

    # ------------------------------------------------------------------
    # HTTP（板端拉取）模式
    # ------------------------------------------------------------------
    def _run_http(self):
        self._log("== HTTP 板端拉取 OTA ==", "cyan")
        http_port = int(self.cfg.get("http_port", 8081))
        board_ip = self.cfg.get("board_ip", "192.168.10.10")
        srv = HttpOtaServer(self._pkg_path, http_port)
        try:
            url = srv.start(board_ip)
            self._log(f"HTTP 服务已启动: {url}", "cyan")
            # 板端 ota http 命令接受 <ip[:port]>/<path>，去掉协议前缀
            cmd = f"ota http {url.split('://', 1)[-1]}\r"
            self._http_t0 = time.perf_counter()
            ctl = self.cfg.get("ctl_mode", "uart")
            if ctl == "uart":
                self._http_via_uart(cmd)
            elif ctl == "tcp":
                self._http_via_tcp(cmd)
            else:
                raise TransportError(f"未知控制通道: {ctl}")
            if self._stop_flag:
                return
            self._stage(self.STAGE_VERIFYING)
            if self.cfg.get("verify_http"):
                self._verify_http_status()
        finally:
            srv.stop()

    def _http_via_uart(self, cmd: str):
        """经 UART 控制通道（COM9 shell）下发拉取命令并解析进度。"""
        ctl = UartTransport(self.cfg["ctl_port"],
                            int(self.cfg.get("ctl_baud", 115200)))
        ctl.open()
        try:
            ctl.drain()
            ctl.write(cmd.encode())
            self._log(f"已下发: {cmd.strip()}", "cyan")
            self._stage(self.STAGE_DOWNLOADING)
            buf = b""
            deadline = time.time() + 120
            done = False
            while time.time() < deadline and not self._stop_flag:
                n = ctl._ser.in_waiting if ctl.is_open else 0
                if n:
                    d = ctl.read(n)
                    if d:
                        buf += d
                        text = buf.decode("utf-8", "replace")
                        # 解析板端进度行 [OTA-HTTP] xxx/yyy
                        for m in re.finditer(r"\[OTA-HTTP\] (\d+)/(\d+)", text):
                            self._report(int(m.group(1)), int(m.group(2)))
                        if "download complete" in text or "Ota_End" in text:
                            dt = time.perf_counter() - self._http_t0
                            self._log(
                                f"✅ 板端拉取完成: {len(self._pkg)}B "
                                f"@ {dt:.1f}s ({len(self._pkg)/1024/max(dt,0.1):.0f}KB/s)，"
                                f"正在复位切换...", "green")
                            done = True
                            break
                        if "-> -" in text or "failed" in text.lower():
                            raise TransportError("板端 HTTP 拉取失败")
                        buf = buf[-4096:]   # 只保留尾部，防缓冲膨胀
                time.sleep(0.01)
            if not done and not self._stop_flag:
                raise TransportError("等待板端 HTTP 拉取超时")
        finally:
            ctl.close()

    def _http_via_tcp(self, cmd: str):
        """经 TCP 控制通道（:9000 控制台）下发拉取命令并解析进度。"""
        sock = socket.create_connection((self.cfg["ctl_ip"], 9000), timeout=5)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(0.5)
        try:
            time.sleep(0.3)
            try:
                sock.recv(4096)   # 消费欢迎语/提示符
            except socket.timeout:
                pass
            sock.sendall(cmd.encode())
            self._log(f"已下发: {cmd.strip()}", "cyan")
            self._stage(self.STAGE_DOWNLOADING)
            buf = b""
            deadline = time.time() + 120
            done = False
            while time.time() < deadline and not self._stop_flag:
                try:
                    d = sock.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    break   # 设备复位断开
                if not d:
                    break
                buf += d
                text = buf.decode("utf-8", "replace")
                for m in re.finditer(r"\[OTA-HTTP\] (\d+)/(\d+)", text):
                    self._report(int(m.group(1)), int(m.group(2)))
                if "download complete" in text or "Ota_End" in text:
                    dt = time.perf_counter() - self._http_t0
                    self._log(
                        f"✅ 板端拉取完成: {len(self._pkg)}B "
                        f"@ {dt:.1f}s ({len(self._pkg)/1024/max(dt,0.1):.0f}KB/s)，"
                        f"正在复位切换...", "green")
                    done = True
                    break
                if "-> -" in text or "failed" in text.lower():
                    raise TransportError("板端 HTTP 拉取失败")
                buf = buf[-4096:]
            if not done and not self._stop_flag:
                raise TransportError("等待板端 HTTP 拉取超时")
        finally:
            sock.close()

    # ------------------------------------------------------------------
    # 升级后验证
    # ------------------------------------------------------------------
    def _verify_boot_log(self):
        """抓取 COM9 启动日志，核验 'last build' 与 'Boot complete'。"""
        port = self.cfg.get("debug_port", "")
        baud = int(self.cfg.get("debug_baud", 115200))
        if not port:
            return
        self._log(f"验证启动日志（{port}@{baud}）...", "cyan")
        try:
            dbg = UartTransport(port, baud)
            dbg.open()
            try:
                buf = b""
                # 窗口需覆盖 BOOT 升级(~11s)+APP 启动+打印：8s 太短会错过
                deadline = time.time() + 20
                while time.time() < deadline:
                    n = dbg._ser.in_waiting if dbg.is_open else 0
                    if n:
                        buf += dbg.read(n)
                    time.sleep(0.02)
                text = buf.decode("utf-8", "replace")
                ok_build = f"last build {self._build_no}" in text
                ok_boot = "Boot complete" in text
                if ok_build:
                    self._log(f"✅ 启动日志确认 build #{self._build_no}", "green")
                else:
                    self._log("⚠️ 未在启动日志中找到构建号（可能已错过启动窗口）",
                              "orange")
                self.device_info.emit({"boot_complete": ok_boot,
                                       "build_confirmed": ok_build})
            finally:
                dbg.close()
        except Exception as e:  # noqa: BLE001
            self._log(f"启动日志验证失败: {e}", "orange")

    def _verify_http_status(self, ip: str = ""):
        """轮询板端 :8080 状态页，确认重启完成并读取版本/资源。"""
        import urllib.request
        ip = ip or self.cfg.get("tcp_ip", "")
        if not ip:
            return
        url = f"http://{ip}:8080/api/status"
        deadline = time.time() + 30
        while time.time() < deadline and not self._stop_flag:
            try:
                with urllib.request.urlopen(url, timeout=1) as resp:
                    if resp.status == 200:
                        data = resp.read().decode("utf-8", "replace")
                        import json
                        st = json.loads(data)
                        uptime = int(st.get("uptime_s", 999))
                        # 升级全程 <60s：uptime<60 即可确认刚重启
                        if uptime < 60 and st.get("link", 0) == 1:
                            self._log(
                                f"✅ 新固件已运行: v{st.get('ver')} · "
                                f"IP {st.get('ip')} · 堆 {st.get('heap_free')}B",
                                "green")
                            self.device_info.emit(st)
                            return
            except Exception:
                pass
            time.sleep(1.0)
        self._log("⚠️ 状态页验证超时（下载与 BOOT 阶段已确认，不影响结论）",
                  "orange")

    def _probe_tcp_status(self, ip: str = "", timeout: float = 8.0) -> bool:
        """快速探测 :9020 STATUS：确认新固件 APP 的 OTA 服务已在线。"""
        from .transport import TcpTransport, TransportError
        ip = ip or self.cfg.get("tcp_ip", "")
        if not ip:
            return False
        t = TcpTransport(ip, int(self.cfg.get("tcp_port", 9020)))
        deadline = time.time() + timeout
        while time.time() < deadline and not self._stop_flag:
            try:
                t.open()
                r = t.cmd(4, b"", timeout=3)   # STATUS
                t.close()
                if r is not None:
                    self._log(f"✅ 新固件在线（:9020 STATUS state={r[0]}）",
                              "green")
                    return True
            except (TransportError, OSError):
                try:
                    t.close()
                except Exception:
                    pass
            time.sleep(1.0)
        return False

    # ------------------------------------------------------------------
    # 速率计量
    # ------------------------------------------------------------------
    def _report(self, done: int, total: int):
        # 必须用 perf_counter（QPC）：monotonic 在此平台是 GetTickCount64，
        # 15.6ms 分辨率导致逐块 dt 恒为 0，速率/ETA 永远算不出来
        now = time.perf_counter()
        if self._last_t > 0:
            dt = now - self._last_t
            if dt > 0:
                inst = (done - self._last_b) / dt
                self._speed = 0.8 * self._speed + 0.2 * inst
        else:
            self._speed = 0.0
        self._last_t = now
        self._last_b = done
        eta = (total - done) / self._speed if self._speed > 0 else 0.0
        self.progress.emit(done, total, int(self._speed), eta)

    def _log(self, msg: str, color: str = "default"):
        self.log.emit(msg, color)

    def _stage(self, st: str):
        self.stage.emit(st)
