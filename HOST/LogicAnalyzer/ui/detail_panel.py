"""细节面板：选中样本/区间的二进制、十六进制、十进制、ASCII"""
from __future__ import annotations

import numpy as np

from pyqtgraph.Qt import QtCore, QtWidgets

from model.trace import TraceData


# 位视图每通道位串的预览上限：超长区间只展示前 N bit，避免窗口被撑爆
BIT_PREVIEW_MAX = 128


class DetailPanel(QtWidgets.QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.trace: TraceData | None = None
        self._range: tuple[int, int] | None = None
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(6, 6, 6, 6)

        head = QtWidgets.QLabel("光标 / 区间详情")
        head.setObjectName("panelTitle")
        layout.addWidget(head)

        baud_row = QtWidgets.QWidget()
        baud_hl = QtWidgets.QHBoxLayout(baud_row)
        baud_hl.setContentsMargins(0, 0, 0, 0)
        baud_hl.addWidget(QtWidgets.QLabel("波特率(Hz)"))
        self._baud_edit = QtWidgets.QLineEdit("115200")
        self._baud_edit.setFixedWidth(90)
        self._baud_edit.textChanged.connect(lambda _: self._populate())
        baud_hl.addWidget(self._baud_edit)
        baud_hl.addStretch(1)
        layout.addWidget(baud_row)

        self._sample_lbl = QtWidgets.QLabel("样本: -")
        self._sample_lbl.setTextInteractionFlags(
            QtCore.Qt.TextSelectableByMouse)
        layout.addWidget(self._sample_lbl)

        self._bit_table = QtWidgets.QTableWidget(0, 4)
        self._bit_table.setHorizontalHeaderLabels(
            ["通道", "bit时长(µs)", "样本区间", "位序列(1字符=1bit)"])
        header = self._bit_table.horizontalHeader()
        header.setSectionResizeMode(0, QtWidgets.QHeaderView.ResizeToContents)
        header.setSectionResizeMode(1, QtWidgets.QHeaderView.ResizeToContents)
        header.setSectionResizeMode(2, QtWidgets.QHeaderView.ResizeToContents)
        header.setSectionResizeMode(3, QtWidgets.QHeaderView.Stretch)
        self._bit_table.setEditTriggers(
            QtWidgets.QAbstractItemView.NoEditTriggers)
        self._bit_table.setWordWrap(False)
        layout.addWidget(self._bit_table, 1)

        self._value_lbl = QtWidgets.QLabel("")
        self._value_lbl.setTextInteractionFlags(
            QtCore.Qt.TextSelectableByMouse)
        layout.addWidget(self._value_lbl)

        self._measure_lbl = QtWidgets.QLabel("")
        self._measure_lbl.setTextInteractionFlags(
            QtCore.Qt.TextSelectableByMouse)
        self._measure_lbl.setStyleSheet("color: #81C784;")
        layout.addWidget(self._measure_lbl)

    def set_trace(self, trace: TraceData):
        self.trace = trace

    def show_sample(self, idx: int):
        if self.trace is None:
            return
        s = self.trace.samples[idx]
        bits = f"{s & 0xFF:08b}"
        self._sample_lbl.setText(
            f"样本 {idx}  @ {idx / self.trace.rate * 1e6:.3f} µs\n"
            f"原始值: 0x{s:08X}  二进制: {bits}")
        self._value_lbl.setText(
            f"CH0..7 电平: {[(s >> ch) & 1 for ch in range(8)]}")

    def _run_length_bits(self, ch: int, a: int, b: int):
        """把区间内采样点做游程压缩：返回 [(电平, 样本数), ...]（物理电平段）。"""
        bits = self.trace.channel(ch)[a:b].astype(int)
        runs = []
        cur = int(bits[0])
        cnt = 1
        for v in bits[1:]:
            if int(v) == cur:
                cnt += 1
            else:
                runs.append((cur, cnt))
                cur = int(v)
                cnt = 1
        runs.append((cur, cnt))
        return runs

    def _protocol_bits(self, ch: int, a: int, b: int, spb: float):
        """按波特率把区间切成协议 bit 网格：每 bit 取中心采样点，不合并连续相同。
        返回的字符串中 1 字符 = 1 个协议 bit。"""
        nbits = int((b - a) / spb)
        if nbits <= 0:
            return ""
        idx = (a + (np.arange(nbits) + 0.5) * spb).astype(int)
        idx = np.clip(idx, 0, self.trace.count - 1)
        vals = (self.trace.samples[idx] >> ch) & 1
        return "".join(str(int(v)) for v in vals)

    def _level_summary(self, ch: int, a: int, b: int):
        runs = self._run_length_bits(ch, a, b)
        parts = [f"{'H' if lv else 'L'}({cnt / self.trace.rate * 1e6:.1f}µs)"
                 for lv, cnt in runs[:8]]
        summary = " ".join(parts)
        if len(runs) > 8:
            summary += f" … 共 {len(runs)} 段"
        return summary

    def show_range(self, a: int, b: int):
        if self.trace is None or b <= a:
            return
        self._range = (a, b)
        self._populate()

    def _populate(self):
        if self.trace is None or self._range is None:
            return
        a, b = self._range
        span = b - a
        try:
            baud = int(self._baud_edit.text())
            if baud <= 0:
                raise ValueError
        except ValueError:
            baud = 115200
        spb = self.trace.rate / baud
        bit_us = 1e6 / baud
        nch = min(self.trace.nchannels, 8)
        self._bit_table.setRowCount(nch)
        for ch in range(nch):
            bits = self._protocol_bits(ch, a, b, spb)
            total_bits = len(bits)
            preview = bits[:BIT_PREVIEW_MAX]
            if total_bits > BIT_PREVIEW_MAX:
                preview += f" … (共 {total_bits} bit，仅预览前 {BIT_PREVIEW_MAX})"
            self._bit_table.setItem(ch, 0, QtWidgets.QTableWidgetItem(f"CH{ch}"))
            self._bit_table.setItem(
                ch, 1, QtWidgets.QTableWidgetItem(f"{bit_us:.1f}"))
            self._bit_table.setItem(ch, 2,
                                    QtWidgets.QTableWidgetItem(f"{a} ~ {b}"))
            self._bit_table.setItem(ch, 3, QtWidgets.QTableWidgetItem(preview))

        # 十六进制/十进制/ASCII（取 CH0 位流的前 8 bit）
        bits0 = self._protocol_bits(0, a, b, spb)
        m8 = int(bits0[:8], 2) if len(bits0) >= 8 else None
        lines = [f"区间 {a}~{b}  ({span} 样本, "
                 f"{span / self.trace.rate * 1e6:.3f} µs)"]
        lines.append(f"协议bit模式: 每字符 = 1 bit，bit宽 = {bit_us:.2f} µs "
                     f"(@{baud} Hz，可改波特率刷新)")
        lines.append(f"电平段: {self._level_summary(0, a, b)}")
        if m8 is not None:
            lines.append(f"CH0 位流前8位: {bits0[:8]} = 0x{m8:02X} = {m8}")
        self._value_lbl.setText("\n".join(lines))

    def show_measure(self, lines):
        self._measure_lbl.setText("\n".join(lines) if lines else "")
