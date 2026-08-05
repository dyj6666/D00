"""细节面板：选中区间的位视图 + 协议字节解析（HEX/十进制/ASCII）。

蓝框选中区间后自动展示：
  - 每通道按波特率切分的协议 bit 串（1 字符 = 1 bit，可复制）；
  - 协议感知的字节解析（UART/SPI 帧 -> HEX 序列与 ASCII）；
  - 区间摘要、电平段时长、一键复制。
"""
from __future__ import annotations

import numpy as np

from pyqtgraph.Qt import QtCore, QtGui, QtWidgets

from la import decoders as dec
from model.trace import TraceData


# 位视图每通道位串的预览上限：超长区间只展示前 N bit，避免窗口被撑爆
BIT_PREVIEW_MAX = 128
BYTE_PREVIEW_MAX = 16


class DetailPanel(QtWidgets.QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.trace: TraceData | None = None
        self._range: tuple[int, int] | None = None
        self._proto: str | None = None
        self._cfg = None
        self._mono = QtGui.QFont("Consolas")
        self._mono.setPointSize(9)
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(6, 6, 6, 6)

        head = QtWidgets.QLabel("区间详情 / 位视图")
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

        self._summary_lbl = QtWidgets.QLabel("区间: -")
        self._summary_lbl.setTextInteractionFlags(
            QtCore.Qt.TextSelectableByMouse)
        layout.addWidget(self._summary_lbl)

        self._bit_table = QtWidgets.QTableWidget(0, 5)
        self._bit_table.setHorizontalHeaderLabels(
            ["通道", "bit时长(µs)", "样本区间",
             "位序列(1字符=1bit)", "字节HEX"])
        header = self._bit_table.horizontalHeader()
        header.setSectionResizeMode(0, QtWidgets.QHeaderView.ResizeToContents)
        header.setSectionResizeMode(1, QtWidgets.QHeaderView.ResizeToContents)
        header.setSectionResizeMode(2, QtWidgets.QHeaderView.ResizeToContents)
        header.setSectionResizeMode(3, QtWidgets.QHeaderView.Stretch)
        header.setSectionResizeMode(4, QtWidgets.QHeaderView.ResizeToContents)
        self._bit_table.setEditTriggers(
            QtWidgets.QAbstractItemView.NoEditTriggers)
        self._bit_table.setWordWrap(False)
        layout.addWidget(self._bit_table, 1)

        self._value_lbl = QtWidgets.QLabel("")
        self._value_lbl.setTextInteractionFlags(
            QtCore.Qt.TextSelectableByMouse)
        layout.addWidget(self._value_lbl)

        copy_row = QtWidgets.QWidget()
        copy_hl = QtWidgets.QHBoxLayout(copy_row)
        copy_hl.setContentsMargins(0, 0, 0, 0)
        self._copy_bits_btn = QtWidgets.QPushButton("复制位串(CH0)")
        self._copy_bits_btn.clicked.connect(lambda: self._copy_col(3))
        self._copy_hex_btn = QtWidgets.QPushButton("复制HEX(CH0)")
        self._copy_hex_btn.clicked.connect(lambda: self._copy_col(4))
        copy_hl.addWidget(self._copy_bits_btn)
        copy_hl.addWidget(self._copy_hex_btn)
        copy_hl.addStretch(1)
        layout.addWidget(copy_row)

        self._measure_lbl = QtWidgets.QLabel("")
        self._measure_lbl.setTextInteractionFlags(
            QtCore.Qt.TextSelectableByMouse)
        self._measure_lbl.setStyleSheet("color: #81C784;")
        layout.addWidget(self._measure_lbl)

    def set_trace(self, trace: TraceData):
        self.trace = trace
        self._proto = None
        self._cfg = None
        self._summary_lbl.setText("区间: -")

    def set_protocol(self, proto: str | None, cfg) -> None:
        """由主窗口传入当前解码协议与配置，用于字节解析与位串切分。"""
        self._proto = proto
        self._cfg = cfg
        self._populate()

    def show_sample(self, idx: int):
        if self.trace is None:
            return
        s = self.trace.samples[idx]
        bits = f"{s & 0xFF:08b}"
        self._summary_lbl.setText(
            f"样本 {idx}  @ {idx / self.trace.rate * 1e6:.3f} µs  "
            f"0x{s:08X}  {bits}")

    # ---------------- 位串与解析 ----------------
    def _protocol_bits(self, ch: int, a: int, b: int, spb: float):
        """按波特率把区间切成协议 bit 网格：每 bit 取中心采样点，不合并。"""
        nbits = int((b - a) / spb)
        if nbits <= 0:
            return ""
        idx = (a + (np.arange(nbits) + 0.5) * spb).astype(int)
        idx = np.clip(idx, 0, self.trace.count - 1)
        vals = (self.trace.samples[idx] >> ch) & 1
        return "".join(str(int(v)) for v in vals)

    def _run_length_bits(self, ch: int, a: int, b: int):
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

    def _level_summary(self, ch: int, a: int, b: int):
        runs = self._run_length_bits(ch, a, b)
        parts = [f"{'H' if lv else 'L'}({cnt / self.trace.rate * 1e6:.1f}µs)"
                 for lv, cnt in runs[:8]]
        summary = " ".join(parts)
        if len(runs) > 8:
            summary += f" … 共 {len(runs)} 段"
        return summary

    def _decode_bytes(self, a: int, b: int):
        """协议感知：返回样本区间 [a,b) 内的完整字节序列（起始于区间内的帧）。"""
        if self.trace is None or self._proto is None or self._cfg is None:
            return []
        try:
            if self._proto == "UART":
                pkts = dec.decode_uart(self.trace.samples,
                                       self.trace.rate, self._cfg)
            elif self._proto == "SPI":
                pkts = dec.decode_spi(self.trace.samples,
                                      self.trace.rate, self._cfg)
            else:
                return []
        except Exception:  # noqa: BLE001
            return []
        return [p.data[0] for p in pkts
                if p.data and a <= p.start < b]

    # ---------------- 展示 ----------------
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
        bytes_by_ch: dict[int, list[int]] = {}
        for ch in range(nch):
            bits = self._protocol_bits(ch, a, b, spb)
            total_bits = len(bits)
            preview = bits[:BIT_PREVIEW_MAX]
            if total_bits > BIT_PREVIEW_MAX:
                preview += f" … (共 {total_bits} bit，仅预览前 {BIT_PREVIEW_MAX})"
            cell = QtWidgets.QTableWidgetItem(f"CH{ch}")
            self._bit_table.setItem(ch, 0, cell)
            self._bit_table.setItem(
                ch, 1, QtWidgets.QTableWidgetItem(f"{bit_us:.1f}"))
            self._bit_table.setItem(ch, 2,
                                    QtWidgets.QTableWidgetItem(f"{a} ~ {b}"))
            bit_item = QtWidgets.QTableWidgetItem(preview)
            bit_item.setFont(self._mono)
            self._bit_table.setItem(ch, 3, bit_item)

            bytes_ = self._decode_bytes(a, b)
            if bytes_:
                hex_str = " ".join(f"{x:02X}" for x in bytes_[:BYTE_PREVIEW_MAX])
                if len(bytes_) > BYTE_PREVIEW_MAX:
                    hex_str += f" … 共{len(bytes_)}字节"
            else:
                hex_str = "-"
            hex_item = QtWidgets.QTableWidgetItem(hex_str)
            hex_item.setFont(self._mono)
            self._bit_table.setItem(ch, 4, hex_item)
            bytes_by_ch[ch] = bytes_

        lines = [f"区间 {a}~{b}  ({span} 样本, "
                 f"{span / self.trace.rate * 1e6:.3f} µs)"]
        if self._proto and bytes_by_ch.get(0):
            bs = bytes_by_ch[0]
            ascii_str = "".join(chr(x) if 32 <= x < 127 else "."
                                for x in bs[:BYTE_PREVIEW_MAX])
            lines.append(f"CH0 字节: "
                         f"{' '.join(f'{x:02X}' for x in bs[:BYTE_PREVIEW_MAX])}")
            lines.append(f"          ASCII: {ascii_str}")
        lines.append(f"bit宽 {bit_us:.2f}µs @{baud}Hz，1字符=1bit")
        lines.append(f"电平段: {self._level_summary(0, a, b)}")
        self._value_lbl.setText("\n".join(lines))

    def _copy_col(self, col: int):
        if self.trace is None or self._range is None:
            return
        item = self._bit_table.item(0, col)
        if item is None:
            return
        text = item.text()
        QtWidgets.QApplication.clipboard().setText(text)
        self._measure_lbl.setText(f"已复制: {text[:48]}…" if len(text) > 48
                                  else f"已复制: {text}")

    def show_measure(self, lines):
        self._measure_lbl.setText("\n".join(lines) if lines else "")
