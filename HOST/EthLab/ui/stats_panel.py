# -*- coding: utf-8 -*-
"""协议统计：计数 + 实时速率曲线。"""

import collections
import time

from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtGui import QColor, QFont, QPainter
from PyQt5.QtWidgets import (QGridLayout, QHBoxLayout, QLabel, QPushButton,
                             QVBoxLayout, QWidget)


class RateChart(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._samples = collections.deque(maxlen=240)
        self.setMinimumHeight(170)

    def add_sample(self, pps, kbps):
        self._samples.append((pps, kbps))
        self.update()

    def reset(self):
        self._samples.clear()
        self.update()

    def paintEvent(self, ev):
        p = QPainter(self)
        p.fillRect(self.rect(), QColor("#0f131b"))
        p.setPen(QColor("#232a38"))
        for gy in range(0, self.height(), 24):
            p.drawLine(0, gy, self.width(), gy)
        if not self._samples:
            p.setPen(QColor("#5b6577"))
            p.drawText(self.rect(), Qt.AlignCenter, "等待抓帧数据...")
            return
        w = self.width()
        h = self.height()
        max_pps = max((s[0] for s in self._samples), default=1) or 1
        max_kb = max((s[1] for s in self._samples), default=1) or 1
        n = len(self._samples)

        def poly(vals, scale):
            pts = []
            for i, v in enumerate(vals):
                x = i * w / max(n - 1, 1)
                y = h - (v / scale) * (h - 24) - 6
                pts.append((int(x), int(y)))
            return pts

        p.setPen(QColor("#4fc3f7"))
        pts = poly([s[0] for s in self._samples], max_pps)
        for i in range(1, len(pts)):
            p.drawLine(pts[i - 1][0], pts[i - 1][1], pts[i][0], pts[i][1])
        p.setPen(QColor("#ffb74d"))
        pts = poly([s[1] for s in self._samples], max_kb)
        for i in range(1, len(pts)):
            p.drawLine(pts[i - 1][0], pts[i - 1][1], pts[i][0], pts[i][1])
        p.setPen(QColor("#9aa5b5"))
        p.drawText(8, 16, "帧/s(青) max=%d    吞吐 KB/s(橙) max=%.1f" %
                   (max_pps, max_kb))


class StatsPanel(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._counters = {
            "总帧数": 0, "TX": 0, "RX": 0, "ARP": 0, "IPv4": 0, "IPv6": 0,
            "ICMP": 0, "TCP": 0, "UDP": 0, "其他协议": 0, "校验和错误": 0,
            "截断帧": 0, "总字节": 0,
        }
        self._labels = {}
        self._history = collections.deque(maxlen=65536)
        self._build_ui()
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(1000)

    def _build_ui(self):
        grid = QGridLayout()
        order = ["总帧数", "TX", "RX", "ARP", "IPv4", "IPv6", "ICMP", "TCP",
                 "UDP", "其他协议", "校验和错误", "截断帧", "总字节"]
        for i, key in enumerate(order):
            lab = QLabel("%s: 0" % key)
            lab.setStyleSheet(
                "background:#141820;border:1px solid #2e3748;"
                "border-radius:4px;padding:6px 10px;font-weight:bold;")
            self._labels[key] = lab
            grid.addWidget(lab, i // 4, i % 4)

        btn = QPushButton("清零统计")
        btn.clicked.connect(self.reset)
        grid.addWidget(btn, 3, 3)

        self._chart = RateChart()
        lay = QVBoxLayout(self)
        lay.addLayout(grid)
        lay.addWidget(self._chart, 1)

    def on_frame(self, fr):
        c = self._counters
        c["总帧数"] += 1
        c[fr.direction] += 1
        proto = fr.l4 or fr.proto
        if proto in c:
            c[proto] += 1
        else:
            c["其他协议"] += 1
        if any("FAIL" in f.desc for f in fr.fields):
            c["校验和错误"] += 1
        if fr.truncated:
            c["截断帧"] += 1
        c["总字节"] += len(fr.raw)
        self._history.append((time.time(), len(fr.raw)))
        self._update_labels()

    def _update_labels(self):
        for key, lab in self._labels.items():
            lab.setText("%s: %s" % (key, self._counters[key]))

    def _tick(self):
        now = time.time()
        cutoff = now - 2.0
        while self._history and self._history[0][0] < cutoff:
            self._history.popleft()
        span = max(now - (self._history[0][0] if self._history else now), 0.5)
        pps = len(self._history) / span
        byt = sum(b for _, b in self._history)
        kbps = byt / span / 1024.0 * 8.0
        self._chart.add_sample(pps, kbps)

    def reset(self):
        for k in self._counters:
            self._counters[k] = 0
        self._history.clear()
        self._update_labels()
        self._chart.reset()
