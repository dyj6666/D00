import serial
import struct
import threading
import time
from queue import Queue
from .protocol import parse_stream, build_frame

class Transport:
    def __init__(self, port: str, baudrate: int = 921600):
        self.serial = serial.Serial(port, baudrate, timeout=0.1)
        self.rx_buf = bytearray()
        self.lock = threading.Lock()
        self.rx_queue = Queue()
        self.tx_queue = Queue()
        self._running = False
        self._rx_thread = None
        self._tx_thread = None

    def start(self):
        self._running = True
        self._rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._rx_thread.start()
        self._tx_thread = threading.Thread(target=self._tx_loop, daemon=True)
        self._tx_thread.start()

    def stop(self):
        self._running = False
        if self.serial.is_open:
            self.serial.close()

    def send(self, cmd: int, payload: bytes = b""):
        self.tx_queue.put(build_frame(cmd, payload))

    def get_frame(self, timeout=0.01):
        try:
            return self.rx_queue.get(timeout=timeout)
        except:
            return None

    def _rx_loop(self):
        while self._running:
            try:
                data = self.serial.read(self.serial.in_waiting or 1)
                if data:
                    with self.lock:
                        self.rx_buf.extend(data)
                    while True:
                        frame = parse_stream(self.rx_buf)
                        if frame is None:
                            break
                        length = struct.unpack("<H", frame[3:5])[0]
                        cmd = frame[2]
                        payload = frame[5:5+length]
                        self.rx_queue.put((cmd, payload))
            except Exception as e:
                print(f"RX error: {e}")
                time.sleep(0.001)

    def _tx_loop(self):
        while self._running:
            try:
                frame = self.tx_queue.get(timeout=0.01)
                self.serial.write(frame)
                self.serial.flush()
            except Exception as e:
                from queue import Empty
                if isinstance(e, Empty):
                    pass  # 队列空，正常等待
                else:
                    import traceback
                    print("TX error details:")
                    traceback.print_exc()