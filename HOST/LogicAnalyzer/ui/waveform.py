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
            brush=pg.mkBrush(80, 160, 255, 30),
            pen=pg.mkPen("#64B5F6", width=1))
        self._region.setZValue(15)
        self._region.hide()
        self._region.sigRegionChanged.connect(self._on_region)
        self.addItem(self._region)

    # ---------- 数据 ----------
    def set_trace(self, trace: TraceData):
        self.trace = trace
        self.clear()
        self._curves = []
        self._labels = []
        if trace is None:
            return
        x = trace.time_axis_us()
        for ch in range(min(trace.nchannels, 8)):
            y = (8 - ch) * 2.0 + trace.channel(ch)
            curve = self.plot(x, y, pen=pg.mkPen(CH_COLORS[ch], width=1.4),
                              antialias=False,
                              connect="finite")
            curve.setDownsampling(auto=True, method="peak")
            curve.setClipToView(True)
            self._curves.append(curve)
            label = pg.TextItem(f"CH{ch}", color=CH_COLORS[ch],
                                anchor=(1, 0.5))
            label.setPos(x[0], (8 - ch) * 2.0 + 0.5)
            self.addItem(label)
            self._labels.append(label)
        self.setYRange(0.5, 17.5)
        self._cursor.setPos(x[0])
        # 默认测量区覆盖约 2 个 PWM 周期（2000 样本 @100kHz = 20ms）
        self._region.setRegion([x[0], x[min(2000, len(x) - 1)]])
        # 默认显示测量区（可拖动），让用户直接看到频率/占空比
        self._region.show()

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
        self.setYRange(0.5, 17.5)

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
