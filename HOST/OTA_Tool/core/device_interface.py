# core/device_interface.py

import serial

class SerialDevice:
    """串口设备接口，便于未来扩展其他通信方式"""
    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 0.5):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self._ser = None

    def connect(self):
        self._ser = serial.Serial(self.port, self.baudrate, timeout=self.timeout)

    def disconnect(self):
        if self._ser and self._ser.is_open:
            self._ser.close()

    def write(self, data: bytes):
        self._ser.write(data)

    def read(self, size: int = 1) -> bytes:
        return self._ser.read(size)

    def readline(self) -> str:
        return self._ser.readline().decode('utf-8', errors='ignore')

    @property
    def is_open(self) -> bool:
        return self._ser and self._ser.is_open