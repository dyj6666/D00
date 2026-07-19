from PyQt5.QtWidgets import QWidget, QVBoxLayout, QTabWidget
import pyqtgraph as pg
from collections import deque
import time
import numpy as np

COLORS = ['#FF0000', '#00FF00', '#0000FF', '#FF00FF', '#00FFFF', '#FFA500', '#800080']

class PlotPanel(QWidget):
    def __init__(self, max_points=10000, update_interval=50):
        super().__init__()
        self.max_points = max_points
        layout = QVBoxLayout(self)
        self.tab_widget = QTabWidget()
        layout.addWidget(self.tab_widget)

        self.plots = {}             # plot_name -> PlotItem
        self.curves = {}            # vid -> PlotDataItem
        self.buffers = {}           # vid -> (deque, deque)
        self.curve_plot_map = {}    # vid -> plot_name
        self.start_time = time.time()
        self.color_idx = 0
        self.is_paused = False

        self.add_plot("主图表")

        self.timer = pg.QtCore.QTimer()
        self.timer.timeout.connect(self._update_plots)
        self.timer.start(update_interval)

        # 鼠标悬停标签（动态添加到当前图表）
        self.cursor_label = pg.TextItem("", anchor=(0,1), color=(200,200,200))
        # 不预先添加到任何图表，在 on_mouse_moved 中动态添加

        # 全局鼠标移动信号（只连接主图表的场景，但可扩展到所有图表，为简洁暂用主图表场景）
        self.proxy = pg.SignalProxy(self.plots["主图表"].scene().sigMouseMoved, rateLimit=30, slot=self.on_mouse_moved)

    def add_plot(self, name: str):
        if name in self.plots:
            return
        plot_widget = pg.PlotWidget(title=name)
        plot_item = plot_widget.getPlotItem()
        plot_item.showGrid(x=True, y=True)
        plot_item.setLabel('bottom', '时间', units='s')
        plot_item.setLabel('left', '值')
        plot_item.addLegend()
        plot_item.addLine(y=0, pen=pg.mkPen(color='r', width=1, style=pg.QtCore.Qt.DashLine))
        self.tab_widget.addTab(plot_widget, name)
        self.plots[name] = plot_item
        self.tab_widget.setCurrentWidget(plot_widget)

        # 双击锁定/解锁Y轴
        plot_item.vb.mouseDoubleClickEvent = lambda ev, pi=plot_item: self._toggle_y_auto(ev, pi)

    def _toggle_y_auto(self, ev, plot_item):
        """切换 Y 轴自动缩放"""
        if plot_item.vb.autoRangeEnabled()[1]:
            plot_item.enableAutoRange(y=False)
        else:
            plot_item.enableAutoRange(y=True)
        ev.accept()

    def get_plot_names(self):
        return list(self.plots.keys())

    def add_curve(self, vid: int, name: str, color=None, plot_name: str = None):
        if plot_name is None:
            plot_name = "主图表"
        if plot_name not in self.plots:
            self.add_plot(plot_name)
        if vid in self.curves:
            self.remove_curve(vid)
        if color is None:
            color = COLORS[self.color_idx % len(COLORS)]
            self.color_idx += 1
        pen = pg.mkPen(color=color, width=2)
        curve = self.plots[plot_name].plot([], [], name=name, pen=pen)
        self.curves[vid] = curve
        self.buffers[vid] = (deque(maxlen=self.max_points), deque(maxlen=self.max_points))
        self.curve_plot_map[vid] = plot_name

    def remove_curve(self, vid: int):
        if vid in self.curves:
            plot_name = self.curve_plot_map.get(vid, "主图表")
            if plot_name in self.plots:
                self.plots[plot_name].removeItem(self.curves[vid])
            del self.curves[vid]
            del self.buffers[vid]
            if vid in self.curve_plot_map:
                del self.curve_plot_map[vid]

            # 清理空图表
            if plot_name != "主图表":
                has_curve = any(p == plot_name for p in self.curve_plot_map.values())
                if not has_curve and plot_name in self.plots:
                    for i in range(self.tab_widget.count()):
                        if self.tab_widget.tabText(i) == plot_name:
                            self.tab_widget.removeTab(i)
                            break
                    del self.plots[plot_name]

    def add_data(self, vid: int, value: float):
        if vid not in self.buffers:
            return
        t = time.time() - self.start_time
        ts_buf, val_buf = self.buffers[vid]
        ts_buf.append(t)
        val_buf.append(float(value))

    def pause(self):
        self.is_paused = True

    def resume(self):
        self.is_paused = False

    def _update_plots(self):
        if self.is_paused:
            return
        for plot_name, plot_item in self.plots.items():
            all_y = []
            latest_t = time.time() - self.start_time
            has_curve = False
            for vid, curve in list(self.curves.items()):
                if self.curve_plot_map.get(vid) != plot_name:
                    continue
                has_curve = True
                if vid not in self.buffers:
                    continue
                ts_buf, val_buf = self.buffers[vid]
                if not ts_buf:
                    continue
                x_list = list(ts_buf)
                y_list = list(val_buf)
                if len(x_list) == 1:
                    x_list.append(x_list[0] + 0.001)
                    y_list.append(y_list[0])
                curve.setData(x_list, y_list)
                all_y.extend(y_list)

            if all_y:
                y_min = min(all_y)
                y_max = max(all_y)
                if y_max == y_min:
                    plot_item.setYRange(y_min - 0.5, y_max + 0.5)
                else:
                    margin = (y_max - y_min) * 0.1
                    plot_item.setYRange(y_min - margin, y_max + margin)

            if has_curve and latest_t > 0:
                plot_item.setXRange(max(0, latest_t - 10), latest_t + 1)

    def on_mouse_moved(self, evt):
        if not self.plots:
            return
        idx = self.tab_widget.currentIndex()
        if idx < 0:
            return
        current_plot_name = self.tab_widget.tabText(idx)
        plot_item = self.plots.get(current_plot_name)
        if plot_item is None:
            return

        pos = evt[0]
        if plot_item.sceneBoundingRect().contains(pos):
            mouse_point = plot_item.vb.mapSceneToView(pos)
            x, y = mouse_point.x(), mouse_point.y()
            self.cursor_label.setText(f"x: {x:.3f}, y: {y:.3f}")
            self.cursor_label.setPos(mouse_point.x(), mouse_point.y())

            # 如果标签不在当前图表，则移动过去
            if self.cursor_label not in plot_item.items:
                for p in self.plots.values():
                    if self.cursor_label in p.items:
                        p.removeItem(self.cursor_label)
                        break
                plot_item.addItem(self.cursor_label, ignoreBounds=True)
        else:
            self.cursor_label.setText("")