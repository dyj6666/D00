"""解码数据包面板"""
from __future__ import annotations

from pyqtgraph.Qt import QtCore, QtGui, QtWidgets


class PacketPanel(QtWidgets.QWidget):
    packetClicked = QtCore.Signal(int, int)

    def __init__(self, parent=None):
        super().__init__(parent)
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(6, 6, 6, 6)

        head = QtWidgets.QLabel("解码结果")
        head.setObjectName("panelTitle")
        layout.addWidget(head)

        self.table = QtWidgets.QTableWidget(0, 6)
        self.table.setHorizontalHeaderLabels(
            ["#", "类型", "起始样本", "结束样本", "数据(HEX)", "说明"])
        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.horizontalHeader().setSectionResizeMode(
            5, QtWidgets.QHeaderView.Stretch)
        self.table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        self.table.setSelectionBehavior(QtWidgets.QAbstractItemView.SelectRows)
        self.table.itemSelectionChanged.connect(self._emit_selection)
        layout.addWidget(self.table)

    def clear(self):
        self.table.setRowCount(0)

    def set_packets(self, packets, rate: int):
        self.table.setRowCount(len(packets))
        for i, p in enumerate(packets):
            values = [
                str(i + 1),
                p.kind,
                str(p.start),
                str(p.end),
                p.data.hex(" ") if p.data else "",
                p.info,
            ]
            for col, v in enumerate(values):
                item = QtWidgets.QTableWidgetItem(v)
                if p.kind.startswith(("START", "STOP", "ADDR", "SPI_FRAME", "FRAME")):
                    item.setForeground(QtGui.QBrush(
                        QtGui.QColor("#64B5F6")))
                self.table.setItem(i, col, item)

    def _emit_selection(self):
        rows = self.table.selectionModel().selectedRows()
        if not rows:
            return
        row = rows[0].row()
        try:
            a = int(self.table.item(row, 2).text())
            b = int(self.table.item(row, 3).text())
        except (TypeError, ValueError):
            return
        self.packetClicked.emit(a, b)
