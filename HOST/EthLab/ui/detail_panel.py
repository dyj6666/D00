# -*- coding: utf-8 -*-
"""帧详情面板：逐字节结构图 + 字段树 + Hex 视图。"""

from PyQt5.QtGui import QColor
from PyQt5.QtWidgets import (QHBoxLayout, QHeaderView, QLabel, QPlainTextEdit,
                             QPushButton, QScrollArea, QTabWidget,
                             QTreeWidget, QTreeWidgetItem, QVBoxLayout,
                             QWidget)

from eth.decoders import layer_color
from ui.byte_view import ByteView


def hexdump(data, width=16):
    lines = []
    for i in range(0, len(data), width):
        chunk = data[i:i + width]
        hexpart = " ".join("%02X" % x for x in chunk)
        hexpart += "   " * (width - len(chunk))
        asc = "".join(chr(x) if 32 <= x < 127 else "." for x in chunk)
        lines.append("%04X  %-*s  %s" % (i, width * 3 - 1, hexpart, asc))
    return "\n".join(lines)


class DetailPanel(QTabWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._frame = None
        self._build_ui()

    def _build_ui(self):
        self.byte_view = ByteView()
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(self.byte_view)

        zoom = QHBoxLayout()
        b_small = QPushButton("缩小")
        b_big = QPushButton("放大")
        b_fit = QPushButton("适应窗口")
        b_fit.setToolTip("让整帧宽度适配当前窗口")
        zoom.addWidget(b_small)
        zoom.addWidget(b_big)
        zoom.addWidget(b_fit)
        zoom.addStretch(1)
        self.hover_label = QLabel("悬停/点击字节查看字段详情")
        zoom.addWidget(self.hover_label, 1)

        page = QWidget()
        lay = QVBoxLayout(page)
        lay.setContentsMargins(4, 4, 4, 4)
        lay.addLayout(zoom)
        lay.addWidget(scroll, 1)
        self.addTab(page, "字节结构")

        self.tree = QTreeWidget()
        self.tree.setHeaderLabels(["字段", "偏移", "长度", "值", "说明"])
        self.tree.header().setSectionResizeMode(0, QHeaderView.ResizeToContents)
        self.tree.header().setSectionResizeMode(3, QHeaderView.ResizeToContents)
        self.tree.header().setSectionResizeMode(4, QHeaderView.Stretch)
        self.addTab(self.tree, "字段树")

        self.hexview = QPlainTextEdit()
        self.hexview.setReadOnly(True)
        self.hexview.setLineWrapMode(QPlainTextEdit.NoWrap)
        self.addTab(self.hexview, "Hex")

        b_small.clicked.connect(
            lambda: self.byte_view.set_cell_width(self.byte_view.cell_width() - 2))
        b_big.clicked.connect(
            lambda: self.byte_view.set_cell_width(self.byte_view.cell_width() + 2))
        b_fit.clicked.connect(self._fit)
        self.byte_view.fieldHovered.connect(self._on_hover)

    def _fit(self):
        if self._frame is None or not len(self._frame.raw):
            return
        view = self.byte_view.parentWidget()
        avail = (view.viewport().width() if view is not None else 900) - 40
        w = max(6, min(32, int(avail / len(self._frame.raw))))
        self.byte_view.set_cell_width(w)

    def _on_hover(self, f):
        if f is None:
            self.hover_label.setText("悬停/点击字节查看字段详情")
        else:
            self.hover_label.setText(
                "%s | 偏移 0x%X (%d) | %d 字节 | 值: %s%s" % (
                    f.name, f.offset, f.offset, f.length, f.value,
                    (" | " + f.desc) if f.desc else ""))

    def show_frame(self, fr):
        self._frame = fr
        self.byte_view.set_frame(fr)
        self.tree.clear()
        layers = {}
        for f in fr.fields:
            if f.layer not in layers:
                item = QTreeWidgetItem([f.layer, "", "", "", ""])
                item.setForeground(0, QColor(layer_color(f.layer)))
                self.tree.addTopLevelItem(item)
                layers[f.layer] = item
            child = QTreeWidgetItem([f.name, "0x%X (%d)" % (f.offset, f.offset),
                                     "%d" % f.length, f.value, f.desc])
            layers[f.layer].addChild(child)
        self.tree.expandAll()
        self.hexview.setPlainText(hexdump(fr.raw))
