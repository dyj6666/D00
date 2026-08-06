"""数字波形控件：8 通道堆叠、缩放平移、十字光标、区间测量"""
from __future__ import annotations

import numpy as np
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtGui

from model.trace import TraceData


CH_COLORS = [
    "#4FC3F7", "#81C784", "#FFB74D", "#F06292",
    "#BA68C8", "#FFF176", "#4DB6AC", "#E57373",
]

ANNO_STYLE = {
    "start": ("#EF5350", (239, 83, 80, 48)),
    "data":  ("#42A5F5", (66, 165, 245, 40)),
    "stop":  ("#FFA726", (255, 167, 38, 44)),
    "addr":  ("#AB47BC", (171, 71, 188, 40)),
    "ack":   ("#26A69A", (38, 166, 154, 40)),
    "frame": ("#66BB6A", (102, 187, 106, 34)),
    "byte":  ("#FFE082", (255, 224, 130, 32)),
    "miso":  ("#FF8A65", (255, 138, 101, 40)),
    "cs":    ("#FFD54F", (255, 213, 79, 30)),
    "idle":  ("#90A4AE", (144, 164, 174, 24)),
}
ANNO_BORDER = (255, 235, 150, 70)


class ProtocolOverlay(pg.GraphicsObject):
    """协议标注单图层：把所有位/字节色块、采样沿竖线、箭头、标签
    绘制在同一个 QGraphicsItem 内，拖动/缩放只需一次变换，保证流畅。"""

    def __init__(self, annos, rate: int, nch: int):
        super().__init__()
        self._annos = annos
        self._rate = rate
        self._nch = nch
        x_max = 1.0
        for a in annos:
            x1 = a.end / rate * 1e6
            if x1 > x_max:
                x_max = x1
        self._bounds = QtCore.QRectF(0, 0, x_max, nch * 2.0 + 2.0)
        self._font_bit = QtGui.QFont("Consolas", 7)
        self._font_byte = QtGui.QFont("Consolas", 8)
        self._font_byte.setBold(True)

    def boundingRect(self):
        return self._bounds

    def paint(self, p, option, widget=None):
        p.setRenderHint(QtGui.QPainter.Antialiasing, False)
        er = option.exposedRect
        x_lo, x_hi = er.left(), er.right()
        rate = self._rate
        n = self._nch
        us = 1e6 / rate
        # 文本只在 bit 宽度达到阈值时绘制（缩放太小则只画色块/竖线，保证流畅）
        scale = p.transform().m11()
        bit_px = scale * (31 * us)      # 当前缩放下一个 bit 的像素宽
        show_bit_labels = bit_px >= 12.0       # 逐位标签：放大后才显示
        show_byte_labels = bit_px * 8.0 >= 55.0  # 字节HEX+二进制：更早显示

        # 第一遍：字节级色块（底层淡黄）
        for a in self._annos:
            if a.kind != "byte" or a.ch is None or a.ch >= n:
                continue
            x0 = a.start / rate * 1e6
            x1 = a.end / rate * 1e6
            if x1 < x_lo or x0 > x_hi:
                continue
            bot = (n - a.ch) * 2.0
            p.fillRect(QtCore.QRectF(x0, bot, max(x1 - x0, us * 0.5), 2.0),
                       QtGui.QColor(255, 224, 130, 30))

        # 第二遍：位级色块（相邻同色合并）/竖线/箭头 + 全部标签
        def flush_merge():
            if merge is not None:
                p.fillRect(QtCore.QRectF(merge[2], (n - merge[0]) * 2.0,
                                         max(merge[3] - merge[2], us * 0.3),
                                         2.0),
                           QtGui.QColor(*ANNO_STYLE[merge[1]][1]))

        merge = None   # (ch, kind, x0, x1)
        for a in self._annos:
            if a.ch is None or a.ch >= n:
                continue
            x0 = a.start / rate * 1e6
            x1 = a.end / rate * 1e6
            if x1 < x_lo or x0 > x_hi:
                continue
            top = (n - a.ch) * 2.0 + 2.0
            bot = (n - a.ch) * 2.0
            color, _ = ANNO_STYLE.get(a.kind, ("#B0BEC5", (176, 190, 197, 30)))
            if a.kind == "byte":
                flush_merge()
                merge = None
                if show_byte_labels:
                    p.setFont(self._font_byte)
                    p.setPen(QtGui.QColor("#FFE082"))
                    p.drawText(
                        QtCore.QRectF(x0, top - 0.85, max(x1 - x0, us), 0.6),
                        QtCore.Qt.AlignHCenter | QtCore.Qt.AlignBottom, a.label)
                continue
            # 位级：相邻同通道同类型首尾相连则合并色块
            if (merge is not None and merge[0] == a.ch
                    and merge[1] == a.kind and abs(merge[3] - x0) < us * 0.5):
                merge = (a.ch, a.kind, merge[2], max(merge[3], x1))
            else:
                flush_merge()
                merge = (a.ch, a.kind, x0, x1)
            # 采样沿竖线 + 顶部箭头（逐条绘制）
            if bit_px >= 2.0:
                p.setPen(QtGui.QPen(QtGui.QColor(*ANNO_BORDER), 0))
                p.drawLine(QtCore.QPointF(x0, bot), QtCore.QPointF(x0, top))
                p.setBrush(QtGui.QColor(color))
                p.setPen(QtCore.Qt.NoPen)
                tri = QtGui.QPolygonF([
                    QtCore.QPointF(x0, top - 0.7),
                    QtCore.QPointF(x0 - 0.45, top - 1.25),
                    QtCore.QPointF(x0 + 0.45, top - 1.25),
                ])
                p.drawPolygon(tri)
            if show_bit_labels:
                p.setFont(self._font_bit)
                p.setPen(QtGui.QColor(color))
                p.drawText(QtCore.QRectF(x0 - 7, bot + 0.05, 14, 0.7),
                           QtCore.Qt.AlignHCenter | QtCore.Qt.AlignTop, a.label)
        flush_merge()


class WaveformWidget(pg.PlotWidget):
    sampleSelected = QtCore.Signal(int)      # 光标样本下标
    regionSelected = QtCore.Signal(int, int)  # 区间起止
    doubleClickedAt = QtCore.Signal(int)      # 双击位置样本下标

    def __init__(self, parent=None):
        super().__init__(parent, background="#0F1420")
        self.trace: TraceData | None = None
        self._curves = []
        self._labels = []
        self._anno_items: list = []
        self._cursor: pg.InfiniteLine | None = None
        self._region: pg.LinearRegionItem | None = None

        self.setMenuEnabled(False)
        self.showGrid(x=True, y=False, alpha=0.25)
        self.getPlotItem().hideButtons()
        self.setMouseEnabled(x=True, y=False)

        self._cursor = pg.InfiniteLine(angle=90, movable=True,
                                       pen=pg.mkPen("#FFFFFF", width=1,
                                                    style=QtCore.Qt.DashLine))
        self._cursor.setZValue(20)
        self._cursor.sigPositionChanged.connect(self._on_cursor)
        self.addItem(self._cursor)

        self._region = pg.LinearRegionItem(
            [0, 100], movable=True,
            brush=pg.mkBrush(80, 170, 255, 45),
            pen=pg.mkPen("#42A5F5", width=2))
        self._region.setZValue(30)
        self._region.hide()
        self._region.sigRegionChanged.connect(self._on_region)
        self.addItem(self._region)

    # ---------- 数据 ----------
    def set_trace(self, trace: TraceData):
        self.trace = trace
        self.clear()
        # clear() 会移除全部 item，需重新挂回光标与测量区
        self.addItem(self._cursor)
        self.addItem(self._region)
        self._curves = []
        self._labels = []
        self._anno_items = []
        if trace is None:
            return
        x = trace.time_axis_us()
        n = min(trace.nchannels, 8)
        for ch in range(n):
            y = (n - ch) * 2.0 + trace.channel(ch)
            curve = self.plot(x, y, pen=pg.mkPen(CH_COLORS[ch], width=1.4),
                              antialias=False,
                              connect="finite")
            curve.setDownsampling(auto=True, method="peak")
            curve.setClipToView(True)
            self._curves.append(curve)
            label = pg.TextItem(f"CH{ch}", color=CH_COLORS[ch],
                                anchor=(1, 0.5))
            label.setPos(x[0], (n - ch) * 2.0 + 0.5)
            label.setZValue(25)
            self.addItem(label)
            self._labels.append(label)
        self.setYRange(0.5, n * 2.0 + 2.0)
        self._cursor.setPos(x[0])
        # 默认测量区：波形中间三分之一，明显可见、可拖动
        n = len(x)
        self._region.setRegion([x[n // 3], x[min(2 * n // 3, n - 1)]])
        # 默认显示测量区（可拖动），让用户直接看到频率/占空比
        self._region.show()

    # ---------- 协议位标注 ----------
    def set_protocol_annotations(self, annos) -> None:
        """把协议标注合并为单个图层绘制，拖动/缩放保持流畅。"""
        self.clear_protocol_annotations()
        if not annos or self.trace is None:
            return
        overlay = ProtocolOverlay(annos, self.trace.rate,
                                  min(self.trace.nchannels, 8))
        overlay.setZValue(9)
        self.addItem(overlay)
        self._anno_items = [overlay]

    def clear_protocol_annotations(self) -> None:
        for item in self._anno_items:
            self.removeItem(item)
        self._anno_items = []

    def set_channel_names(self, names: list) -> None:
        """更新波形左侧通道标签文本（如 CH0 -> CH0=SCK），位置保持不变。"""
        for ch, label in enumerate(self._labels):
            if ch < len(names):
                label.setText(names[ch])

    # ---------- 交互 ----------
    def show_cursor(self, show: bool):
        self._cursor.setVisible(show)

    def show_region(self, show: bool, a: int = 0, b: int = 100):
        if self.trace is None:
            return
        x = self.trace.time_axis_us()
        self._region.setRegion([x[a], x[min(b, len(x) - 1)]])
        self._region.setVisible(show)

    def highlight(self, a: int, b: int):
        if self.trace is None:
            return
        x = self.trace.time_axis_us()
        self._region.setRegion([x[a], x[min(b, len(x) - 1)]])
        self._region.show()
        self._region.setZValue(15)

    def fit_all(self):
        if self.trace is None:
            return
        x = self.trace.time_axis_us()
        self.setXRange(x[0], x[-1], padding=0.01)
        n = min(self.trace.nchannels, 8)
        self.setYRange(0.5, n * 2.0 + 2.0)

    def set_region_samples(self, a: int, b: int):
        """程序化框选 [a, b) 样本区间（如双击定位协议帧）。"""
        if self.trace is None:
            return
        x = self.trace.time_axis_us()
        self._region.setRegion([x[a], x[min(b, len(x) - 1)]])
        self._region.show()

    def mouseDoubleClickEvent(self, ev):
        if self.trace is not None:
            vb = self.getPlotItem().vb
            pos = vb.mapSceneToView(ev.pos())
            idx = int(round(pos.x() * self.trace.rate / 1e6))
            idx = max(0, min(self.trace.count - 1, idx))
            self.doubleClickedAt.emit(idx)
        super().mouseDoubleClickEvent(ev)

    def _on_cursor(self, line):
        if self.trace is None:
            return
        x = line.value()
        idx = int(round((x - 0) * self.trace.rate / 1e6))
        idx = max(0, min(self.trace.count - 1, idx))
        self.sampleSelected.emit(idx)

    def _on_region(self, region):
        if self.trace is None:
            return
        x = self.trace.time_axis_us()
        a = int(np.searchsorted(x, region.getRegion()[0]))
        b = int(np.searchsorted(x, region.getRegion()[1]))
        self.regionSelected.emit(a, b)

    def measure(self, ch: int, a: int, b: int):
        """区间内频率/占空比测量（窗口无关的稳健算法）

        频率：相邻同向边沿间距的中位数（窗口覆盖不到整数周期也准确）；
        占空比：高/低电平持续时间的众数比（避免窗口切割造成偏差）。
        """
        if self.trace is None or b <= a:
            return None
        bits = self.trace.channel(ch)[a:b]
        rate = self.trace.rate
        n = len(bits)
        if n < 4:
            return None

        rising = np.flatnonzero((bits[1:] == 1) & (bits[:-1] == 0)) + 1
        falling = np.flatnonzero((bits[1:] == 0) & (bits[:-1] == 1)) + 1

        # 周期：优先用上升沿间距中位数，不足则用下降沿
        period = 0.0
        if rising.size >= 2:
            period = float(np.median(np.diff(rising)))
        elif falling.size >= 2:
            period = float(np.median(np.diff(falling)))
        freq = rate / period if period > 0 else 0.0

        # 占空比：高低电平持续时间的众数比（抗窗口切割）
        runs_high: list = []
        runs_low: list = []
        cur = int(bits[0])
        run = 1
        for v in bits[1:]:
            if int(v) == cur:
                run += 1
            else:
                (runs_high if cur else runs_low).append(run)
                cur = int(v)
                run = 1
        (runs_high if cur else runs_low).append(run)

        if runs_high and runs_low:
            h = float(np.median(runs_high))
            l = float(np.median(runs_low))
            duty = h / (h + l)
        else:
            duty = float(bits.sum()) / n

        return {"ch": ch, "samples": b - a,
                "us": (b - a) / rate * 1e6,
                "duty": duty, "rising": int(rising.size),
                "freq": freq}
