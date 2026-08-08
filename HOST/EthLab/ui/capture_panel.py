# -*- coding: utf-8 -*-
"""实时抓包面板：UDP 7778 监听板端抓帧流 + 帧列表 + 过滤/搜索。"""

import json
import socket
import time

from PyQt5.QtCore import QThread, Qt, pyqtSignal
from PyQt5.QtWidgets import (QCheckBox, QComboBox, QHBoxLayout, QHeaderView,
                             QLabel, QLineEdit, QPushButton, QSpinBox,
                             QTableWidget, QTableWidgetItem, QVBoxLayout,
                             QWidget)

from eth import decoders
from eth.pcap import write_pcap


class CaptureListener(QThread):
    datagram = pyqtSignal(bytes)

    def __init__(self, port=7778, parent=None):
        super().__init__(parent)
        self.port = port
        self._running = False
        self._sock = None

    def run(self):
        try:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self._sock.bind(("0.0.0.0", self.port))
            self._sock.settimeout(0.3)
        except OSError as e:
            self.datagram.emit(b"ERR:" + str(e).encode("utf-8", "replace"))
            return
        self._running = True
        while self._running:
            try:
                data, _ = self._sock.recvfrom(65535)
                if data:
                    self.datagram.emit(data)
            except socket.timeout:
                continue
            except OSError:
                break
        s = self._sock
        self._sock = None
        if s is not None:
            try:
                s.close()
            except OSError:
                pass

    def stop(self):
        self._running = False
        s = self._sock
        self._sock = None
        if s is not None:
            try:
                s.close()
            except OSError:
                pass


def parse_capture_datagram(data):
    """板端抓帧载荷 -> (direction, raw, orig_len, truncated)。"""
    if len(data) < 4:
        return None
    d = data[0]
    flags = data[1]
    orig = int.from_bytes(data[2:4], "big")
    raw = data[4:]
    direction = "TX" if d == 1 else ("RX" if d == 2 else "?")
    if len(raw) > orig:
        raw = raw[:orig]
    return direction, raw, orig, bool(flags & 1)


class CapturePanel(QWidget):
    frame_selected = pyqtSignal(object)
    frame_added = pyqtSignal(object)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._frames = []
        self._shown = []
        self._t0 = time.time()
        self._limit = 5000
        self._plain_view = True
        self._build_ui()

    def _build_ui(self):
        bar = QHBoxLayout()
        bar.addWidget(QLabel("过滤:"))
        self.filter = QComboBox()
        self.filter.addItems(["全部", "ARP", "IPv4", "IPv6", "TCP", "UDP",
                              "ICMP", "校验和错误"])
        self.filter.currentTextChanged.connect(self._on_filter_change)
        bar.addWidget(self.filter)
        self.search = QLineEdit()
        self.search.setPlaceholderText("搜索: IP/MAC/端口/摘要/hex 子串...")
        self.search.setClearButtonEnabled(True)
        self.search.textChanged.connect(self._on_filter_change)
        bar.addWidget(self.search, 1)
        self.chk_pause = QCheckBox("暂停")
        bar.addWidget(self.chk_pause)
        self.chk_scroll = QCheckBox("自动滚动")
        self.chk_scroll.setChecked(True)
        bar.addWidget(self.chk_scroll)
        bar.addWidget(QLabel("上限:"))
        self.spin_limit = QSpinBox()
        self.spin_limit.setRange(100, 20000)
        self.spin_limit.setValue(self._limit)
        self.spin_limit.setSingleStep(500)
        self.spin_limit.valueChanged.connect(self._set_limit)
        bar.addWidget(self.spin_limit)
        btn_clear = QPushButton("清空")
        btn_clear.clicked.connect(self.clear)
        bar.addWidget(btn_clear)
        bar.addStretch(1)

        self.table = QTableWidget(0, 8)
        self.table.setHorizontalHeaderLabels(
            ["#", "时间", "方向", "长度", "源", "目的", "协议", "摘要"])
        self.table.horizontalHeader().setSectionResizeMode(7, QHeaderView.Stretch)
        for c in range(7):
            self.table.horizontalHeader().setSectionResizeMode(
                c, QHeaderView.ResizeToContents)
        self.table.setSelectionBehavior(QTableWidget.SelectRows)
        self.table.setEditTriggers(QTableWidget.NoEditTriggers)
        self.table.verticalHeader().setVisible(False)
        self.table.setAlternatingRowColors(True)
        self.table.itemSelectionChanged.connect(self._on_select)

        lay = QVBoxLayout(self)
        lay.addLayout(bar)
        lay.addWidget(self.table, 1)

    # ---- 数据入口 ----

    def add_frame(self, direction, raw, orig_len=None, truncated=False):
        if self.chk_pause.isChecked():
            return
        fr = decoders.decode(direction, raw, orig_len, truncated,
                             ts=time.time())
        self._frames.append(fr)
        self.frame_added.emit(fr)
        if len(self._frames) > self._limit:
            self._frames.pop(0)
            if self._plain_view and self.table.rowCount() > 0:
                self._shown.pop(0)
                self.table.removeRow(0)
            else:
                self._apply_filter()
        if self._matches(fr):
            self._append_row(fr)
        if self.chk_scroll.isChecked():
            self.table.scrollToBottom()

    def count(self):
        return len(self._frames)

    def clear(self):
        self._frames.clear()
        self._shown.clear()
        self.table.setRowCount(0)

    # ---- 过滤/搜索 ----

    def _on_filter_change(self, _=None):
        self._apply_filter()

    def _set_limit(self, v):
        self._limit = v
        while len(self._frames) > self._limit:
            self._frames.pop(0)
        self._apply_filter()

    def _proto_of(self, fr):
        return fr.l4 or fr.proto

    def _matches(self, fr):
        filt = self.filter.currentText()
        if filt == "校验和错误":
            if not any("FAIL" in f.desc for f in fr.fields):
                return False
        elif filt != "全部" and self._proto_of(fr) != filt:
            return False
        q = self.search.text().strip().upper()
        if q:
            hay = " ".join([
                fr.src, fr.dst, self._proto_of(fr), fr.summary,
                fr.direction, fr.raw.hex(" ").upper()])
            if q not in hay:
                return False
        return True

    def _apply_filter(self):
        filt = self.filter.currentText()
        self._plain_view = (filt == "全部" and not self.search.text().strip())
        self._shown = []
        self.table.setRowCount(0)
        for fr in self._frames:
            if self._matches(fr):
                self._append_row(fr)

    def _append_row(self, fr):
        self._shown.append(fr)
        row = self.table.rowCount()
        self.table.insertRow(row)
        length = str(fr.orig_len)
        if fr.truncated:
            length += "/%dB T" % len(fr.raw)
        self.table.setItem(row, 0, QTableWidgetItem(str(row + 1)))
        self.table.setItem(row, 1, QTableWidgetItem("%.3f" % (fr.ts - self._t0)))
        self.table.setItem(row, 2, QTableWidgetItem(fr.direction))
        self.table.setItem(row, 3, QTableWidgetItem(length))
        self.table.setItem(row, 4, QTableWidgetItem(fr.src))
        self.table.setItem(row, 5, QTableWidgetItem(fr.dst))
        self.table.setItem(row, 6, QTableWidgetItem(self._proto_of(fr)))
        self.table.setItem(row, 7, QTableWidgetItem(fr.summary))

    def _on_select(self):
        rows = self.table.selectionModel().selectedRows()
        if not rows:
            return
        idx = rows[0].row()
        if 0 <= idx < len(self._shown):
            self.frame_selected.emit(self._shown[idx])

    # ---- 捕获管理 ----

    def save_json(self, path):
        data = [{"dir": f.direction, "len": f.orig_len,
                 "ts": f.ts, "hex": f.raw.hex()} for f in self._frames]
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=1)
        return len(data)

    def load_json(self, path):
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        n = 0
        for item in data:
            raw = bytes.fromhex(item["hex"])
            self.add_frame(item.get("dir", "TX"), raw, item.get("len"))
            n += 1
        return n

    def export_pcap(self, path):
        write_pcap(path, self._frames)
        return len(self._frames)
