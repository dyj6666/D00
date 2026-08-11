import json
import os

class Config:
    def __init__(self, filepath="config.json"):
        self.filepath = filepath
        self.data = {
            "last_port": "",
            "last_baudrate": "115200",
            "last_file": "",
            "last_version": "1",
            "last_uid": "",
            "last_key": "",
            "last_tcp_ip": "192.168.10.10",
            "last_tcp_port": "9020",
            "last_http_port": "8080",
            "last_ctl": "",
        }
        self.load()

    def load(self):
        if os.path.exists(self.filepath):
            try:
                with open(self.filepath, 'r') as f:
                    self.data = json.load(f)
            except:
                pass

    def save(self):
        with open(self.filepath, 'w') as f:
            json.dump(self.data, f, indent=4)

    def get(self, key, default=None):
        return self.data.get(key, default)

    def set(self, key, value):
        self.data[key] = value
        self.save()
