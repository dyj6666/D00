# core/ota_worker.py
"""OTA 升级工作线程：支持两种模式
  - hostlink : APP 运行时下载（固件经 HOSTLINK 写入 DOWNLOAD 区，BOOT 负责切换）
  - ymodem   : 传统 BOOT YMODEM 升级（加密包直接发送）
"""

import os
import time
import struct

from PyQt5.QtCore import QThread, pyqtSignal

from .hostlink import (FrameParser, OTA_CHUNK_MAX, build_ota_begin,
                       build_ota_data, build_ota_end, build_ota_status)
from .ymodem_sender import YmodemSender, encrypt_and_sign, derive_aes_key_from_uid


class OtaWorker(QThread):
    log_signal = pyqtSignal(str, str)       # 消息, 颜色
    progress_signal = pyqtSignal(int)       # 0-100
    finished_signal = pyqtSignal(bool)      # 成功/失败
    stage_signal = pyqtSignal(str)         # IDLE/DOWNLOADING/VERIFYING/COMMITTED/RUNNING

    def __init__(self, serial_instance, file_path, version,
                 uid_hex, private_key_hex, mode="hostlink"):
        super().__init__()
        self.serial_instance = serial_instance
        self.file_path = file_path
        self.version = version
        self.uid_hex = uid_hex
        self.private_key_hex = private_key_hex
        self.mode = mode
        self._stop_flag = False
        self.chip_id = 0x413
        self.build_no = 1

    def stop(self):
        self._stop_flag = True

    def run(self):
        try:
            self.stage_signal.emit("IDLE")
            lib = load_lib()
            self.chip_id = chip_id_int(lib)
            self.build_no = alloc_build_no(lib)
            add_entry(self.version, self.build_no, self.file_path, "auto")
            if self.mode == "ymodem":
                self._run_ymodem()
            else:
                self._run_hostlink()
        except Exception as e:  # noqa: BLE001
            self.log_signal.emit(f"异常: {e}", "red")
            self.finished_signal.emit(False)

    # ---------------- HOSTLINK 运行时 OTA ----------------
    def _run_hostlink(self):
        self.log_signal.emit("== HOSTLINK 运行时 OTA ==", "cyan")
        self.log_signal.emit(f"固件: {os.path.basename(self.file_path)}", "yellow")
        self.log_signal.emit(f"版本: {self.version}", "yellow")
        self.log_signal.emit(f"构建号: {self.build_no}, 芯片: {self.chip_id:#x}", "yellow")

        # 1) 生成安全固件包（加密 + 签名）
        aes_key = derive_aes_key_from_uid(self.uid_hex)
        pkg_path = os.path.join(os.path.dirname(self.file_path), "_ota_pkg.bin")
        encrypt_and_sign(self.file_path, pkg_path, self.private_key_hex,
                         aes_key.hex(), self.version,
                         self.chip_id, self.build_no)
        with open(pkg_path, "rb") as f:
            pkg = f.read()
        self.log_signal.emit(f"安全包: {len(pkg)} 字节", "cyan")

        ser = self.serial_instance
        parser = FrameParser()

        def cmd(frame, expect_cmd, timeout=1.0, retries=4):
            # 不清缓冲：避免清掉延迟到达的响应；解析器自带重同步
            ser.write(frame)
            parser._buf.clear()
            for _ in range(retries):
                t0 = time.time()
                while time.time() - t0 < timeout:
                    n = ser.in_waiting
                    d = ser.read(n) if n else b''
                    if d:
                        parser.feed(d)
                        for fr in parser.frames():
                            if fr[2] == expect_cmd:
                                return fr
                    if self._stop_flag:
                        return None
                    time.sleep(0.002)
            return None

        # 2) BEGIN
        self.log_signal.emit("发送 BEGIN...", "cyan")
        r = cmd(build_ota_begin(self.version, len(pkg)), 0x08)
        if r is None or r[5] != 0:
            self.log_signal.emit(f"BEGIN 失败: {r[5] if r else '无响应'}", "red")
            self.finished_signal.emit(False)
            return
        self.log_signal.emit("BEGIN OK，下载区已就绪", "green")
        self.stage_signal.emit("DOWNLOADING")

        # 3) 断点续传：查询 APP 已接收进度，从断点继续
        start_off = 0
        r = cmd(build_ota_status(), 0x0B)
        if r is not None:
            st, rx, total = r[5], struct.unpack("<I", r[6:10])[0], \
                struct.unpack("<I", r[10:14])[0]
            if st == 1 and 0 < rx < len(pkg):
                start_off = rx
                self.log_signal.emit(f"检测到断点，从 {rx} 字节续传", "yellow")

        # 4) DATA 分块（逐块确认）
        chunk = OTA_CHUNK_MAX
        for off in range(start_off, len(pkg), chunk):
            if self._stop_flag:
                self.log_signal.emit("已停止", "orange")
                self.finished_signal.emit(False)
                return
            blk = pkg[off:off + chunk]
            r = cmd(build_ota_data(off, blk), 0x09)
            if r is None:
                self.log_signal.emit(f"数据块 {off} 无响应", "red")
                self.finished_signal.emit(False)
                return
            st = r[5]
            if st != 0:
                self.log_signal.emit(f"数据块 {off} 状态 {st}", "red")
                self.finished_signal.emit(False)
                return
            rx = struct.unpack("<I", r[6:10])[0]
            self.progress_signal.emit(int(rx * 100 / len(pkg)))
            time.sleep(0.01)   # 节奏控制：给 APP flash 写留处理时间
            if off % 2400 == 0:
                self.log_signal.emit(f"  已下载 {rx}/{len(pkg)} 字节", "gray")

        # 5) STATUS 确认
        r = cmd(build_ota_status(), 0x0B)
        if r:
            st, rx, total = r[5], struct.unpack("<I", r[6:10])[0], \
                struct.unpack("<I", r[10:14])[0]
            self.log_signal.emit(f"STATUS: state={st} {rx}/{total}", "cyan")

        # 6) END 触发复位切换
        self.log_signal.emit("发送 END，触发 BOOT 切换...", "cyan")
        r = cmd(build_ota_end(), 0x0A)
        if r is None or r[5] != 0:
            self.log_signal.emit(f"END 失败: {r[5] if r else '无响应'}", "red")
            self.finished_signal.emit(False)
            return
        self.log_signal.emit("END OK，设备正在重启并切换新固件", "green")
        self.stage_signal.emit("VERIFYING")
        self.progress_signal.emit(100)
        self.stage_signal.emit("COMMITTED")
        self.finished_signal.emit(True)

    # ---------------- 传统 YMODEM ----------------
    def _run_ymodem(self):
        self.log_signal.emit("== YMODEM 传统升级 ==", "cyan")
        aes_key = derive_aes_key_from_uid(self.uid_hex)
        pkg_path = os.path.join(os.path.dirname(self.file_path), "_ota_pkg.bin")
        encrypt_and_sign(self.file_path, pkg_path, self.private_key_hex,
                         aes_key.hex(), self.version,
                         self.chip_id, self.build_no)
        sender = YmodemSender(serial_instance=self.serial_instance,
                              log_callback=self._on_log,
                              progress_callback=self.progress_signal.emit)
        success = sender.send_file(pkg_path)
        if success:
            self.stage_signal.emit("VERIFYING")
            self.stage_signal.emit("COMMITTED")
        self.finished_signal.emit(success)

    def _on_log(self, message, color="white"):
        self.log_signal.emit(message, color)
