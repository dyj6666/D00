# -*- coding: utf-8 -*-
"""逐字节协议结构视图：把一帧的每个字节按协议字段着色展示。"""

from PyQt5.QtCore import QRectF, QSize, Qt, pyqtSignal
from PyQt5.QtGui import QColor, QFont, QPainter
from PyQt5.QtWidgets import QWidget

from eth.decoders import layer_color


class ByteView(QWidget):
    fieldHovered = pyqtSignal(object)   # Field 或 None
    byteClicked = pyqtSignal(int)       # 字节偏移

    PAD = 12

    def __init__(self, parent=None):
        super().__init__(parent)
        self._frame = None
        self._cell_w = 16
        self._cell_h = 24
        self._label_h = 22
        self._ruler_h = 18
        self._ascii_h = 20
        self._hover_field = None
        self._hover_byte = -1
        self.setMouseTracking(True)
        self.setMinimumHeight(self._label_h + self._ruler_h + self._cell_h +
                              self._ascii_h + 12)

    def set_frame(self, fr):
        self._frame = fr
        self._hover_field = None
        self._hover_byte = -1
        self.update()
        self.updateGeometry()

    def set_cell_width(self, w):
        self._cell_w = max(6, min(32, int(w)))
        self.update()
        self.updateGeometry()

    def cell_width(self):
        return self._cell_w

    def sizeHint(self):
        if self._frame is None:
            return QSize(900, self.minimumHeight())
        w = max(len(self._frame.raw), 16) * self._cell_w + 2 * self.PAD
        h = self._ruler_h + self._label_h + self._cell_h + self._ascii_h + 10
        return QSize(w, h)

    def _x_to_byte(self, x):
        if self._frame is None:
            return -1
        idx = int((x - self.PAD) // self._cell_w)
        if 0 <= idx < len(self._frame.raw):
            return idx
        return -1

    def _field_at(self, byte_idx):
        if self._frame is None:
            return None
        for f in self._frame.fields:
            if f.offset <= byte_idx < f.offset + f.length:
                return f
        return None

    def mouseMoveEvent(self, ev):
        idx = self._x_to_byte(ev.x())
        f = self._field_at(idx) if idx >= 0 else None
        if f is not self._hover_field or idx != self._hover_byte:
            self._hover_field = f
            self._hover_byte = idx
            self.fieldHovered.emit(f)
            self.update()
        super().mouseMoveEvent(ev)

    def leaveEvent(self, ev):
        self._hover_field = None
        self._hover_byte = -1
        self.fieldHovered.emit(None)
        self.update()
        super().leaveEvent(ev)

    def mousePressEvent(self, ev):
        idx = self._x_to_byte(ev.x())
        if idx >= 0:
            self.byteClicked.emit(idx)
        super().mousePressEvent(ev)

    def wheelEvent(self, ev):
        if ev.modifiers() & Qt.ControlModifier:
            delta = ev.angleDelta().y()
            self.set_cell_width(self._cell_w + (2 if delta > 0 else -2))
            ev.accept()
            return
        super().wheelEvent(ev)

    def paintEvent(self, ev):
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#0f131b"))
        if self._frame is None:
            painter.setPen(QColor("#5b6577"))
            painter.drawText(self.rect(), Qt.AlignCenter,
                             "选择一帧后在此查看逐字节结构 (Ctrl+滚轮缩放)")
            return
        fr = self._frame
        n = len(fr.raw)
        cw = self._cell_w
        painter.setRenderHint(QPainter.Antialiasing, False)

        font = QFont("Consolas")
        font.setPixelSize(10)
        painter.setFont(font)
        painter.setPen(QColor("#8a97ad"))
        for i in range(0, n, 8):
            x = self.PAD + i * cw
            painter.drawLine(int(x), self._ruler_h - 4, int(x), self._ruler_h)
            if i % 16 == 0:
                painter.drawText(int(x) + 2, 12, "%04X" % i)

        y = self._ruler_h
        for f in fr.fields:
            x0 = self.PAD + f.offset * cw
            w = f.length * cw
            color = QColor(layer_color(f.layer))
            if f is self._hover_field:
                painter.fillRect(int(x0), y, int(w), self._label_h,
                                 QColor(255, 255, 255, 60))
            painter.setPen(QColor(color))
            painter.drawRect(int(x0), y, int(w) - 1, self._label_h - 1)
            rect = QRectF(x0 + 2, y + 2, w - 4, self._label_h - 4)
            painter.drawText(rect, Qt.AlignCenter, f.name)

        y = self._ruler_h + self._label_h
        hex_font = QFont("Consolas")
        hex_font.setPixelSize(11)
        painter.setFont(hex_font)
        for i in range(n):
            x = self.PAD + i * cw
            rect = QRectF(x, y, cw, self._cell_h)
            f = self._field_at(i)
            if f is not None:
                color = QColor(layer_color(f.layer))
                painter.fillRect(rect, QColor(color.red(), color.green(),
                                              color.blue(), 70))
            if i == self._hover_byte:
                painter.fillRect(rect, QColor(255, 255, 255, 36))
            painter.setPen(QColor("#e8eef7"))
            painter.drawText(rect, Qt.AlignCenter, "%02X" % fr.raw[i])
            if i % 8 == 0:
                painter.setPen(QColor("#3a4a63"))
                painter.drawLine(int(x), y, int(x), y + self._cell_h)

        y = self._ruler_h + self._label_h + self._cell_h
        for i in range(n):
            x = self.PAD + i * cw
            ch = chr(fr.raw[i]) if 32 <= fr.raw[i] < 127 else "."
            rect = QRectF(x, y, cw, self._ascii_h)
            if i == self._hover_byte:
                painter.fillRect(rect, QColor(255, 255, 255, 36))
            painter.setPen(QColor("#9aa5b5"))
            painter.drawText(rect, Qt.AlignCenter, ch)
