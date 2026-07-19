from PyQt5.QtWidgets import (QMainWindow, QSplitter, QToolBar, QAction,
                             QMessageBox, QInputDialog, QFileDialog, QDialog,
                             QFormLayout, QComboBox, QDialogButtonBox, QWidget,
                             QVBoxLayout, QLineEdit)
from PyQt5.QtCore import Qt, QTimer
import serial.tools.list_ports
import numpy as np
from vlink.transport import Transport
from vlink.client import VLinkClient
from ui.variable_tree import VariableTree
from ui.plot_widget import PlotPanel
from utils.config import load_config, save_config
import csv
import os

class SettingsDialog(QDialog):
    """串口设置对话框"""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("串口设置")
        layout = QFormLayout(self)

        self.port_combo = QComboBox()
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo.addItems(ports)
        layout.addRow("端口:", self.port_combo)

        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"])
        self.baud_combo.setCurrentText("921600")
        layout.addRow("波特率:", self.baud_combo)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def get_settings(self):
        return self.port_combo.currentText(), int(self.baud_combo.currentText())


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("VLink Debugger - 实时变量监控")
        self.resize(1400, 900)

        self.transport = None
        self.client = None
        self.var_tree = VariableTree()
        self.plot_panel = PlotPanel()
        self.setup_ui()
        self.restore_state()

        self.timer = QTimer()
        self.timer.timeout.connect(self.update_data)
        self.timer.start(30)

        self.statusBar().showMessage("就绪")

    def setup_ui(self):
        self.toolbar = QToolBar()
        self.addToolBar(self.toolbar)

        self.toolbar.addAction("连接", self.connect_mcu)
        self.toolbar.addAction("断开", self.disconnect_mcu)
        self.toolbar.addAction("发现变量", self.discover)
        self.toolbar.addSeparator()

        self.toolbar.addAction("订阅选中", self.subscribe_selected)
        self.toolbar.addAction("取消订阅", self.unsubscribe_selected)
        self.toolbar.addSeparator()

        self.toolbar.addAction("新建图表", self.add_new_plot)
        self.toolbar.addSeparator()

        self.pause_action = QAction("暂停", self)
        self.pause_action.setCheckable(True)
        self.pause_action.toggled.connect(self.toggle_pause)
        self.toolbar.addAction(self.pause_action)
        self.toolbar.addSeparator()

        self.toolbar.addAction("导出CSV", self.export_csv)

        # 左侧面板：搜索框 + 变量树
        left_panel = QWidget()
        left_layout = QVBoxLayout(left_panel)
        left_layout.setContentsMargins(0, 0, 0, 0)
        self.search_box = QLineEdit()
        self.search_box.setPlaceholderText("搜索变量...")
        self.search_box.textChanged.connect(self.var_tree.filter_variables)
        left_layout.addWidget(self.search_box)
        left_layout.addWidget(self.var_tree)

        splitter = QSplitter(Qt.Horizontal)
        splitter.addWidget(left_panel)
        splitter.addWidget(self.plot_panel)
        self.setCentralWidget(splitter)

        self.var_tree.subscribe_callback = self.subscribe_variable
        self.var_tree.write_callback = self.write_variable
        self.var_tree.move_callback = self.handle_move_curve
        self.var_tree.statistics_callback = self.show_statistics

    # ---------- 通信 ----------
    def connect_mcu(self):
        dialog = SettingsDialog(self)
        if dialog.exec_() != QDialog.Accepted:
            return
        port, baud = dialog.get_settings()
        try:
            self.transport = Transport(port, baud)
            self.transport.start()
            self.client = VLinkClient(self.transport)
            self.statusBar().showMessage(f"已连接 {port} @ {baud}")
            self.discover()
            self.restore_subscriptions()
        except Exception as e:
            QMessageBox.critical(self, "错误", f"连接失败: {e}")

    def disconnect_mcu(self):
        if self.transport:
            self.transport.stop()
        self.statusBar().showMessage("已断开")

    def discover(self):
        if not self.client:
            return
        vars_list = self.client.discover_variables()
        self.var_tree.update_variables(vars_list)
        self.statusBar().showMessage(f"发现 {len(vars_list)} 个变量")

    # ---------- 订阅管理 ----------
    def subscribe_variable(self, vid: int):
        if not self.client:
            return
        var = self.client.variables.get(vid)
        if var:
            self.plot_panel.add_curve(vid, var.name)
            self.client.subscribe([vid])
            self.statusBar().showMessage(f"已订阅 {var.name}")
            self.save_subscriptions()

    def subscribe_selected(self):
        if not self.client:
            QMessageBox.warning(self, "提示", "请先连接MCU")
            return
        ids = self.var_tree.get_selected_ids()
        if not ids:
            QMessageBox.warning(self, "提示", "请先选择变量")
            return
        for vid in ids:
            var = self.client.variables.get(vid)
            if var:
                self.plot_panel.add_curve(vid, var.name)
        self.client.subscribe(ids)
        self.statusBar().showMessage(f"已订阅 {len(ids)} 个变量")
        self.save_subscriptions()

    def unsubscribe_selected(self):
        ids = self.var_tree.get_selected_ids()
        if not ids:
            QMessageBox.warning(self, "提示", "请先选择变量")
            return
        for vid in ids:
            self.plot_panel.remove_curve(vid)
        remaining_ids = list(self.plot_panel.curves.keys())
        if self.client:
            if remaining_ids:
                self.client.subscribe(remaining_ids)
            else:
                self.client.subscribe([])
        self.statusBar().showMessage(f"已取消 {len(ids)} 个变量")
        self.save_subscriptions()

    # ---------- 写入变量 ----------
    def write_variable(self, vid: int):
        if not self.client:
            QMessageBox.warning(self, "提示", "请先连接MCU")
            return
        var = self.client.variables.get(vid)
        if not var:
            return
        if var.permission == 0:
            QMessageBox.warning(self, "提示", "该变量为只读")
            return
        value, ok = QInputDialog.getText(self, "写入变量", f"输入 {var.name} 的新值:")
        if not ok or not value:
            return
        try:
            if var.type == 0:
                val = int(value) & 0xFF
            elif var.type == 1:
                val = int(value)
            elif var.type == 2:
                val = int(value)
            elif var.type == 3:
                val = float(value)
            else:
                QMessageBox.warning(self, "错误", "不支持的类型")
                return
        except ValueError:
            QMessageBox.warning(self, "错误", "格式不正确")
            return
        self.client.write_variable(vid, val)
        self.statusBar().showMessage(f"已写入 {var.name} = {val}")

    # ---------- 数据更新 ----------
    def update_data(self):
        if not self.client:
            return
        data = self.client.get_data_frames(timeout=0)
        for vid, value in data:
            self.plot_panel.add_data(vid, value)
            self.var_tree.update_value(vid, value)

    # ---------- 多图表管理 ----------
    def add_new_plot(self):
        name, ok = QInputDialog.getText(self, "新建图表", "请输入图表名称:")
        if ok and name:
            self.plot_panel.add_plot(name)

    def handle_move_curve(self, action, vid, plot_name=None):
        if action == "get_plot_names":
            return self.plot_panel.get_plot_names()
        elif action == "move" and plot_name:
            if self.client:
                var = self.client.variables.get(vid)
                if var:
                    self.plot_panel.add_curve(vid, var.name, plot_name=plot_name)
                    self.save_subscriptions()

    def toggle_pause(self, checked):
        if checked:
            self.plot_panel.pause()
            self.pause_action.setText("继续")
        else:
            self.plot_panel.resume()
            self.pause_action.setText("暂停")

    # ---------- 数据统计 ----------
    def show_statistics(self, vid: int):
        if vid not in self.plot_panel.buffers:
            QMessageBox.warning(self, "提示", "该变量尚未订阅，无数据")
            return
        ts_buf, val_buf = self.plot_panel.buffers[vid]
        if not val_buf:
            QMessageBox.warning(self, "提示", "数据为空")
            return
        values = np.array(val_buf)
        mean_val = np.mean(values)
        std_val = np.std(values)
        min_val = np.min(values)
        max_val = np.max(values)
        var = self.client.variables.get(vid)
        var_name = var.name if var else f"0x{vid:04X}"
        msg = f"变量: {var_name}\n"
        msg += f"点数: {len(values)}\n"
        msg += f"最小值: {min_val:.4f}\n"
        msg += f"最大值: {max_val:.4f}\n"
        msg += f"平均值: {mean_val:.4f}\n"
        msg += f"标准差: {std_val:.4f}"
        QMessageBox.information(self, "数据统计", msg)

    # ---------- 配置持久化 ----------
    def save_subscriptions(self):
        ids = list(self.plot_panel.curves.keys())
        plot_map = {}
        for vid in ids:
            plot_map[str(vid)] = self.plot_panel.curve_plot_map.get(vid, "主图表")
        config = load_config()
        config["subscriptions"] = ids
        config["plot_map"] = plot_map
        config["geometry"] = {
            "x": self.x(), "y": self.y(),
            "width": self.width(), "height": self.height()
        }
        save_config(config)

    def restore_state(self):
        config = load_config()
        geo = config.get("geometry")
        if geo:
            self.move(geo.get("x", 100), geo.get("y", 100))
            self.resize(geo.get("width", 1400), geo.get("height", 900))

    def restore_subscriptions(self):
        config = load_config()
        ids = config.get("subscriptions", [])
        plot_map = config.get("plot_map", {})
        if ids and self.client:
            for vid in ids:
                var = self.client.variables.get(vid)
                if var:
                    plot_name = plot_map.get(str(vid), "主图表")
                    self.plot_panel.add_curve(vid, var.name, plot_name=plot_name)
            self.client.subscribe(ids)
            self.statusBar().showMessage(f"已恢复 {len(ids)} 个订阅")

    # ---------- 导出 CSV ----------
    def export_csv(self):
        if not self.plot_panel.buffers:
            QMessageBox.warning(self, "提示", "没有数据可导出")
            return
        path, _ = QFileDialog.getSaveFileName(self, "保存CSV", "", "CSV文件 (*.csv)")
        if not path:
            return
        try:
            with open(path, "w", newline="", encoding="utf-8") as f:
                writer = csv.writer(f)
                header = ["时间(s)"]
                vars_info = []
                for vid in self.plot_panel.curves.keys():
                    var = self.client.variables.get(vid)
                    name = var.name if var else f"0x{vid:04X}"
                    header.append(name)
                    vars_info.append((vid, name))
                writer.writerow(header)

                all_timestamps = set()
                for vid in self.plot_panel.buffers:
                    ts_buf, _ = self.plot_panel.buffers[vid]
                    all_timestamps.update(ts_buf)
                all_timestamps = sorted(all_timestamps)

                for t in all_timestamps:
                    row = [f"{t:.6f}"]
                    for vid, _ in vars_info:
                        if vid in self.plot_panel.buffers:
                            ts_buf, val_buf = self.plot_panel.buffers[vid]
                            value = ""
                            for ts, v in zip(ts_buf, val_buf):
                                if abs(ts - t) < 0.001:
                                    value = str(v)
                                    break
                            row.append(value)
                        else:
                            row.append("")
                    writer.writerow(row)
            self.statusBar().showMessage(f"数据已导出到 {os.path.basename(path)}")
        except Exception as e:
            QMessageBox.critical(self, "错误", f"导出失败: {e}")