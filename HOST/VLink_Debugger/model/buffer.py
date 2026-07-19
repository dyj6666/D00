import time

class DataBuffer:
    def __init__(self, maxlen=100000):
        self.maxlen = maxlen
        self.timestamps = []
        self.values = []

    def append(self, value: float):
        t = time.time()
        if len(self.values) >= self.maxlen:
            self.timestamps.pop(0)
            self.values.pop(0)
        self.timestamps.append(t)
        self.values.append(value)

    def get_data(self):
        return self.timestamps.copy(), self.values.copy()

    def clear(self):
        self.timestamps.clear()
        self.values.clear()