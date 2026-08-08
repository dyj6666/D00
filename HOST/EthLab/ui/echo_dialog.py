# -*- coding: utf-8 -*-
"""UDP 回显测试：向板端 7777 端口发 N 包，统计 RTT 与丢包率。"""

import socket
import struct
import time

from PyQt5.QtCore import QThread, pyqtSignal
from PyQt5.QtWidgets import (QDialog, QGridLayout, QHBoxLayout, QHeaderView,
                             QLabel, QLineEdit, QPushButton, QTableWidget,
                             QTableWidgetItem, QVBoxLayout)


class EchoWorker(QThread):
    result = pyqtSignal(int, float)      # seq, rtt_ms (-1 = 超时)
    finished_ok = pyqtSignal(dict)

    def __init__(self, host, port, count, size, interval_ms, parent=None):
        super().__init__(parent)
        self.host = host
        self.port = int(port)
        self.count = int(count)
        self.size = int(size)
        self.interval = float(interval_ms) / 1000.0
        self._stop = False

    def stop(self):
        self._stop = True

    def run(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(0.4)
        rtts = []
        lost = 0
        sent = 0
        for seq in range(1, self.count + 1):
            if self._stop:
                break
            body = max(self.size, 4)
            payload = (struct.pack(">HH", seq, body) +
                       bytes([seq & 0xFF]) * (body - 4))
            t0 = time.perf_counter()
            try:
                sock.sendto(payload, (self.host, self.port))
            except OSError:
                lost += 1
                self.result.emit(seq, -2.0)
                continue
            sent += 1
            ok = False
            deadline = t0 + 1.0
            while time.time() < deadline:
                try:
                    data, _ = sock.recvfrom(4096)
                    if len(data) >= 4 and int.from_bytes(data[:2], "big") == seq:
                        rtt = (time.perf_counter() - t0) * 1000.0
                        rtts.append(rtt)
                        self.result.emit(seq, rtt)
                        ok = True
                        break
                except socket.timeout:
                    continue
            if not ok:
                lost += 1
                self.result.emit(seq, -1.0)
            if self.interval > 0:
                time.sleep(self.interval)
        sock.close()
        self.finished_ok.emit({
            "sent": sent, "ok": len(rtts), "lost": lost,
            "min": min(rtts) if rtts else 0.0,
            "avg": (sum(rtts) / len(rtts)) if rtts else 0.0,
            "max": max(rtts) if rtts else 0.0,
        })


class EchoDialog(QDialog):
    def __init__(self, parent=None, default_ip="192.168.1.10"):
        super().__init__(parent)
        self.setWindowTitle("UDP 回显测试 (板端 :7777)")
        self.setMinimumSize(560, 460)
        self._worker = None

        form = QGridLayout()
        self.ip = QLineEdit(default_ip)
        self.port = QLineEdit("7777")
        self.count = QLineEdit("20")
        self.size = QLineEdit("16")
        self.interval = QLineEdit("50")
        form.addWidget(QLabel("目标 IP:"), 0, 0)
        form.addWidget(self.ip, 0, 1)
        form.addWidget(QLabel("端口:"), 0, 2)
        form.addWidget(self.port, 0, 3)
        form.addWidget(QLabel("包数:"), 1, 0)
        form.addWidget(self.count, 1, 1)
        form.addWidget(QLabel("载荷(字节):"), 1, 2)
        form.addWidget(self.size, 1, 3)
        form.addWidget(QLabel("间隔(ms):"), 2, 0)
        form.addWidget(self.interval, 2, 1)

        self.btn_start = QPushButton("开始")
        self.btn_start.clicked.connect(self._start)
        self.btn_stop = QPushButton("停止")
        self.btn_stop.setEnabled(False)
        self.btn_stop.clicked.connect(self._stop)
        self.btn_close = QPushButton("关闭")
        self.btn_close.clicked.connect(self.accept)
        btns = QHBoxLayout()
        btns.addWidget(self.btn_start)
        btns.addWidget(self.btn_stop)
        btns.addStretch(1)
        btns.addWidget(self.btn_close)

        self.summary = QLabel("未开始")
        self.summary.setStyleSheet(
            "background:#141820;border:1px solid #2e3748;border-radius:4px;"
            "padding:6px;font-weight:bold;")

        self.table = QTableWidget(0, 3)
        self.table.setHorizontalHeaderLabels(["序号", "结果", "RTT (ms)"])
        self.table.horizontalHeader().setSectionResizeMode(
            2, QHeaderView.Stretch)
        self.table.setEditTriggers(QTableWidget.NoEditTriggers)
        self.table.verticalHeader().setVisible(False)

        lay = QVBoxLayout(self)
        lay.addLayout(form)
        lay.addWidget(self.summary)
        lay.addWidget(self.table, 1)
        lay.addLayout(btns)

    def _start(self):
        try:
            count = int(self.count.text())
            size = int(self.size.text())
            interval = float(self.interval.text())
        except ValueError:
            return
        self.table.setRowCount(0)
        self.btn_start.setEnabled(False)
        self.btn_stop.setEnabled(True)
        self._worker = EchoWorker(self.ip.text().strip(),
                                  self.port.text().strip(),
                                  count, size, interval, self)
        self._worker.result.connect(self._on_result)
        self._worker.finished_ok.connect(self._on_done)
        self._worker.start()

    def _stop(self):
        if self._worker is not None:
            self._worker.stop()

    def _on_result(self, seq, rtt):
        row = self.table.rowCount()
        self.table.insertRow(row)
        if rtt >= 0:
            self.table.setItem(row, 0, QTableWidgetItem(str(seq)))
            self.table.setItem(row, 1, QTableWidgetItem("OK"))
            self.table.setItem(row, 2, QTableWidgetItem("%.2f" % rtt))
        else:
            self.table.setItem(row, 0, QTableWidgetItem(str(seq)))
            self.table.setItem(row, 1, QTableWidgetItem("超时/失败"))
            self.table.setItem(row, 2, QTableWidgetItem("-"))

    def _on_done(self, st):
        self.btn_start.setEnabled(True)
        self.btn_stop.setEnabled(False)
        rate = (st["sent"] - st["lost"]) / st["sent"] * 100 if st["sent"] else 0
        self.summary.setText(
            "发送 %d | 成功 %d | 丢失 %d | 成功率 %.1f%% | "
            "RTT min/avg/max = %.2f / %.2f / %.2f ms" % (
                st["sent"], st["ok"], st["lost"], rate,
                st["min"], st["avg"], st["max"]))
        self._worker = None
