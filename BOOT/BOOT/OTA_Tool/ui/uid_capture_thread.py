# ui/uid_capture_thread.py

import time
from PyQt5.QtCore import QThread, pyqtSignal
from core.device_interface import SerialDevice

class UidCaptureThread(QThread):
    uid_captured = pyqtSignal(str)
    log_signal = pyqtSignal(str, str)

    def __init__(self, serial_instance):
        super().__init__()
        self.serial = serial_instance

    def run(self):
        start = time.time()
        while time.time() - start < 5.0:
            line = self.serial.readline().decode('utf-8', errors='ignore')
            if line.startswith("DEV_UID:"):
                uid = line.split(":")[1].strip()
                if len(uid) == 24:
                    self.uid_captured.emit(uid)
                    return
        self.log_signal.emit("未检测到 UID 上报", "orange")