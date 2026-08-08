# -*- coding: utf-8 -*-
"""TCP 控制台面板：连接板端 9000 端口，命令交互 + 遥测流解析。"""

import re
import socket

from PyQt5.QtCore import QThread, Qt, pyqtSignal
from PyQt5.QtWidgets import (QCheckBox, QHBoxLayout, QLabel, QLineEdit,
                             QPlainTextEdit, QPushButton, QVBoxLayout, QWidget)

UPTIME_RE = re.compile(
    r"UPTIME (\d+)s HEAP (\d+)B TASKS (\d+) ETH (\w+) (\d+\.\d+\.\d+\.\d+)")


class TcpClient(QThread):
    data = pyqtSignal(bytes)
    state = pyqtSignal(bool)

    def __init__(self, host, port, parent=None):
        super().__init__(parent)
        self.host = host
        self.port = int(port)
        self._running = False
        self._sock = None

    def run(self):
        try:
            self._sock = socket.create_connection((self.host, self.port),
                                                  timeout=5)
            self._sock.settimeout(0.4)
        except OSError as e:
            self.state.emit(False)
            self.data.emit(("连接失败: %s\r\n" % e).encode("utf-8", "replace"))
            return
        self._running = True
        self.state.emit(True)
        while self._running:
            try:
                d = self._sock.recv(8192)
                if not d:
                    break
                self.data.emit(d)
            except socket.timeout:
                continue
            except OSError:
                break
        self.state.emit(False)

    def send(self, text):
        if self._sock is None or not self._running:
            return False
        try:
            self._sock.sendall(text)
            return True
        except OSError:
            return False

    def stop(self):
        self._running = False
        s = self._sock
        self._sock = None
        if s is not None:
            try:
                s.close()
            except OSError:
                pass


class ConsolePanel(QWidget):
    telemetry = pyqtSignal(dict)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._client = None
        self._partial = b""
        self._history = []
        self._hist_idx = 0
        self._build_ui()

    def _build_ui(self):
        top = QHBoxLayout()
        top.addWidget(QLabel("板端 IP:"))
        self.ip_edit = QLineEdit("192.168.1.10")
        self.ip_edit.setFixedWidth(110)
        self.port_edit = QLineEdit("9000")
        self.port_edit.setFixedWidth(52)
        self.btn_conn = QPushButton("连接")
        self.btn_conn.setCheckable(True)
        self.btn_conn.clicked.connect(self._toggle_conn)
        self.chk_cap = QCheckBox("自动抓帧")
        self.chk_cap.setChecked(True)
        self.chk_cap.setToolTip("连接后自动发送 net cap on")
        top.addWidget(self.ip_edit)
        top.addWidget(self.port_edit)
        top.addWidget(self.btn_conn)
        top.addWidget(self.chk_cap)
        top.addStretch(1)

        quick = QHBoxLayout()
        for name, cmd in (("help", "help"), ("info", "info"),
                          ("sysmon", "sysmon"), ("taskstats", "taskstats"),
                          ("net", "net"), ("抓帧开", "net cap on"),
                          ("抓帧关", "net cap off"),
                          ("stream on", "stream on"), ("stream off", "stream off"),
                          ("led", "led toggle"), ("beep", "beep 200"),
                          ("mpu", "mpu"), ("echo", "echo hello")):
            b = QPushButton(name)
            b.clicked.connect(lambda _=False, c=cmd: self.send_command(c))
            quick.addWidget(b)
        quick.addStretch(1)

        self.output = QPlainTextEdit()
        self.output.setReadOnly(True)
        self.output.setMaximumBlockCount(6000)
        self.output.setLineWrapMode(QPlainTextEdit.NoWrap)

        bottom = QHBoxLayout()
        self.input = QLineEdit()
        self.input.returnPressed.connect(self._on_enter)
        self.btn_send = QPushButton("发送")
        self.btn_send.clicked.connect(lambda: self._on_enter())
        bottom.addWidget(self.input, 1)
        bottom.addWidget(self.btn_send)

        lay = QVBoxLayout(self)
        lay.addLayout(top)
        lay.addLayout(quick)
        lay.addWidget(self.output, 1)
        lay.addLayout(bottom)

    def _toggle_conn(self):
        if self._client is not None and self._client.isRunning():
            self._client.stop()
            self._client.wait(2000)
            self._client = None
            self.btn_conn.setChecked(False)
            self.btn_conn.setText("连接")
            return
        host = self.ip_edit.text().strip()
        port = self.port_edit.text().strip()
        if not host or not port:
            return
        self.btn_conn.setText("断开")
        self._client = TcpClient(host, port, self)
        self._client.data.connect(self._on_data)
        self._client.state.connect(self._on_state)
        self._client.start()

    def _on_state(self, ok):
        if ok:
            self.btn_conn.setChecked(True)
            self.btn_conn.setText("断开")
            if self.chk_cap.isChecked():
                self.send_command("net cap on")
        else:
            self.btn_conn.setChecked(False)
            self.btn_conn.setText("连接")

    def _on_data(self, chunk):
        buf = self._partial + chunk
        self._partial = b""
        lines = buf.split(b"\n")
        if lines and not buf.endswith(b"\n"):
            self._partial = lines.pop()
        for ln in lines:
            line = ln.rstrip(b"\r")
            text = line.decode("utf-8", "replace")
            self.output.appendPlainText(text)
            m = UPTIME_RE.search(text)
            if m:
                self.telemetry.emit({
                    "UPTIME": m.group(1) + "s",
                    "HEAP": m.group(2) + "B",
                    "TASKS": m.group(3),
                    "ETH": m.group(4) + " " + m.group(5),
                })
        sb = self.output.verticalScrollBar()
        sb.setValue(sb.maximum())

    def send_command(self, cmd):
        if not cmd:
            return
        self.output.appendPlainText("> " + cmd)
        if self._client is not None:
            if self._client.send((cmd + "\r\n").encode()):
                self._history.append(cmd)
                self._hist_idx = len(self._history)
        self.input.clear()

    def _on_enter(self):
        self.send_command(self.input.text().strip())

    def keyPressEvent(self, ev):
        if ev.key() == Qt.Key_Up:
            if self._history:
                self._hist_idx = max(0, self._hist_idx - 1)
                self.input.setText(self._history[self._hist_idx])
            return
        if ev.key() == Qt.Key_Down:
            if self._history and self._hist_idx < len(self._history) - 1:
                self._hist_idx += 1
                self.input.setText(self._history[self._hist_idx])
            else:
                self._hist_idx = len(self._history)
                self.input.clear()
            return
        super().keyPressEvent(ev)

    def close_conn(self):
        if self._client is not None:
            self._client.stop()
            self._client.wait(1500)
