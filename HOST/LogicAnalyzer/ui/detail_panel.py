"""细节面板：选中样本/区间的二进制、十六进制、十进制、ASCII"""
from __future__ import annotations

from pyqtgraph.Qt import QtCore, QtWidgets

from model.trace import TraceData


# 位视图每通道位串的预览上限：超长区间只展示前 N bit，避免窗口被撑爆
BIT_PREVIEW_MAX = 128


class DetailPanel(QtWidgets.QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.trace: TraceData | None = None
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(6, 6, 6, 6)

        head = QtWidgets.QLabel("光标 / 区间详情")
        head.setObjectName("panelTitle")
        layout.addWidget(head)

        self._sample_lbl = QtWidgets.QLabel("样本: -")
        self._sample_lbl.setTextInteractionFlags(
            QtCore.Qt.TextSelectableByMouse)
        layout.addWidget(self._sample_lbl)

        self._bit_table = QtWidgets.QTableWidget(0, 4)
        self._bit_table.setHorizontalHeaderLabels(
            ["通道", "电平", "样本区间", "二进制"])
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

    def show_range(self, a: int, b: int):
        if self.trace is None or b <= a:
            return
        nch = min(self.trace.nchannels, 8)
        span = b - a
        preview_b = min(b, a + BIT_PREVIEW_MAX)
        self._bit_table.setRowCount(nch)
        for ch in range(nch):
            bits = self.trace.bit_string(ch, a, preview_b)
            if span > BIT_PREVIEW_MAX:
                bits += f" … (共 {span} bit，仅预览前 {BIT_PREVIEW_MAX} bit)"
            self._bit_table.setItem(ch, 0, QtWidgets.QTableWidgetItem(f"CH{ch}"))
            self._bit_table.setItem(ch, 1, QtWidgets.QTableWidgetItem(""))
            self._bit_table.setItem(ch, 2,
                                    QtWidgets.QTableWidgetItem(f"{a} ~ {b}"))
            self._bit_table.setItem(ch, 3, QtWidgets.QTableWidgetItem(bits))

        # 十六进制/十进制/ASCII（基于预览窗口的 MSB 8 位）
        m8 = self.trace.extract_bytes(0, a, preview_b, "msb")
        lines = [f"区间 {a}~{b}  ({span} 样本, "
                 f"{span / self.trace.rate * 1e6:.3f} µs)"]
        if span > BIT_PREVIEW_MAX:
            lines.append(f"位串过长，仅解析预览前 {BIT_PREVIEW_MAX} bit "
                         f"(已省略 {span - BIT_PREVIEW_MAX} bit)")
        if m8 is not None:
            lines.append(f"CH0 8位: 0x{m8:02X}  十进制: {m8}  "
                         f"ASCII '{chr(m8) if 32 <= m8 < 127 else '.'}'")
        self._value_lbl.setText("\n".join(lines))

    def show_measure(self, lines):
        self._measure_lbl.setText("\n".join(lines) if lines else "")
