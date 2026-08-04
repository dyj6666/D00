# core/ota_worker.py

import os
import time
from PyQt5.QtCore import QThread, pyqtSignal
from .ymodem_sender import YmodemSender, encrypt_and_sign, derive_aes_key_from_uid

class OtaWorker(QThread):
    log_signal = pyqtSignal(str, str)       # 消息, 颜色
    progress_signal = pyqtSignal(int)       # 0-100
    finished_signal = pyqtSignal(bool)      # 成功/失败

    def __init__(self, serial_instance, file_path, version,
                 uid_hex, private_key_hex):
        super().__init__()
        self.serial_instance = serial_instance
        self.file_path = file_path
        self.version = version
        self.uid_hex = uid_hex
        self.private_key_hex = private_key_hex

    def run(self):
        try:
            # 诊断打印
            self.log_signal.emit(f"文件: {self.file_path}", "yellow")
            self.log_signal.emit(f"UID: {self.uid_hex}", "yellow")
            self.log_signal.emit(f"版本: {self.version}", "yellow")
            self.log_signal.emit(f"私钥前缀: {self.private_key_hex[:8]}...", "yellow")

            # 派生密钥
            aes_key = derive_aes_key_from_uid(self.uid_hex)
            self.log_signal.emit(f"派生密钥前8字节: {aes_key[:8].hex().upper()}", "yellow")

            # 加密与签名
            self.log_signal.emit("开始安全处理固件...", "cyan")
            secure_path = "temp_secure.bin"
            encrypt_and_sign(self.file_path, secure_path,
                             self.private_key_hex,
                             aes_key.hex(), self.version)
            self.log_signal.emit("安全固件生成完成", "green")

            # 发送文件（使用已打开的串口对象）
            self.log_signal.emit("连接设备，等待握手...", "cyan")
            sender = YmodemSender(
                serial_instance=self.serial_instance,
                log_callback=self._on_log,
                progress_callback=self._on_progress
            )
            success = sender.send_file(secure_path)

            if success:
                self.log_signal.emit("升级成功！设备即将重启", "green")
            else:
                self.log_signal.emit("升级失败", "red")
            self.finished_signal.emit(success)

            # 保留临时文件用于调试
            # if os.path.exists(secure_path):
            #     os.remove(secure_path)

        except Exception as e:
            self.log_signal.emit(f"异常: {str(e)}", "red")
            self.finished_signal.emit(False)

    def _on_log(self, message, color='white'):
        self.log_signal.emit(message, color)

    def _on_progress(self, percent):
        self.progress_signal.emit(percent)