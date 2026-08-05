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


class WaveformWidget(pg.PlotWidget):
    sampleSelected = QtCore.Signal(int)      # 光标样本下标
    regionSelected = QtCore.Signal(int, int)  # 区间起止

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
    ANNO_STYLE = {
        "start": ("#EF5350", (239, 83, 80, 48)),
        "data":  ("#42A5F5", (66, 165, 245, 36)),
        "stop":  ("#FFA726", (255, 167, 38, 42)),
        "addr":  ("#AB47BC", (171, 71, 188, 36)),
        "ack":   ("#26A69A", (38, 166, 154, 36)),
        "frame": ("#66BB6A", (102, 187, 106, 30)),
        "byte":  ("#FFE082", (255, 224, 130, 30)),
        "cs":    ("#FFD54F", (255, 213, 79, 26)),
        "idle":  ("#90A4AE", (144, 164, 174, 22)),
    }
    ANNO_BORDER = (255, 235, 150, 55)   # 统一浅黄边界线

    def set_protocol_annotations(self, annos) -> None:
        """在波形上按样本区间绘制协议位色块与标签（1 条 = 1 bit/帧）。"""
        self.clear_protocol_annotations()
        if not annos or self.trace is None:
            return
        rate = self.trace.rate
        n = min(self.trace.nchannels, 8)
        y_bit = n * 2.0 + 0.55       # 位级标签（S/Dn/STOP）
        y_byte = n * 2.0 + 1.35      # 字节级汇总标签（HEX+ASCII，更高一层）
        font_small = QtGui.QFont()
        font_small.setPointSize(7)
        font_bold = QtGui.QFont()
        font_bold.setPointSize(8)
        font_bold.setBold(True)
        for a in annos:
            x0 = a.start / rate * 1e6
            x1 = max(a.end / rate * 1e6, x0 + 0.01)
            if a.kind == "byte":
                color, brush = self.ANNO_STYLE["byte"]
                z_r, z_t, y, font = 9, 12, y_byte, font_bold
            else:
                color, brush = self.ANNO_STYLE.get(
                    a.kind, ("#B0BEC5", (176, 190, 197, 30)))
                z_r, z_t, y, font = 10, 11, y_bit, font_small
            region = pg.LinearRegionItem(
                values=(x0, x1), movable=False,
                brush=pg.mkBrush(*brush),
                pen=pg.mkPen(*self.ANNO_BORDER))
            region.setZValue(z_r)
            self.addItem(region)
            label = pg.TextItem(a.label, color=color, anchor=(0.5, 1.0))
            label.setFont(font)
            label.setPos((x0 + x1) / 2, y)
            label.setZValue(z_t)
            self.addItem(label)
            self._anno_items.append((region, label))

    def clear_protocol_annotations(self) -> None:
        for region, label in self._anno_items:
            self.removeItem(region)
            self.removeItem(label)
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
