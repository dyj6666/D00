import sys
import os
import time
import serial
from PyQt5.QtWidgets import (QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
                             QLabel, QLineEdit, QComboBox, QPushButton,
                             QTextEdit, QProgressBar, QFileDialog, QMessageBox,
                             QGroupBox, QFormLayout, QSpinBox, QCheckBox)
from PyQt5.QtCore import Qt
from PyQt5.QtGui import QDragEnterEvent, QDropEvent, QFont
import serial.tools.list_ports
from core.ota_worker import OtaWorker
from ui.uid_capture_thread import UidCaptureThread
from utils.config import Config

VERSION = "v1.0.0"

# 日志颜色常量（深色系，适配浅色背景）
COLOR_OK = "#228B22"
COLOR_ERROR = "#B22222"
COLOR_WARN = "#B8860B"
COLOR_INFO = "#2F4F4F"
COLOR_DEBUG = "#696969"
COLOR_DEFAULT = "#000000"   # 统一黑色

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"D00 固件升级上位机 {VERSION} —— 董衍俊")
        self.resize(720, 620)
        self.config = Config()

        # 应用样式
        style_path = os.path.join(os.path.dirname(__file__), "styles.qss")
        if os.path.exists(style_path):
            with open(style_path, 'r', encoding='utf-8') as f:
                self.setStyleSheet(f.read())

        self.worker = None
        self.uid_thread = None
        self.serial_instance = None
        self.serial_opened = False

        self._init_ui()
        self._load_config()

        # 安装全局事件过滤器，强制捕获拖放
        self.installEventFilter(self)

    def _init_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QVBoxLayout(central)
        main_layout.setSpacing(16)
        main_layout.setContentsMargins(24, 24, 24, 24)

        # ---- 串口设置 ----
        serial_group = QGroupBox("串口设置")
        serial_layout = QHBoxLayout()
        serial_layout.addWidget(QLabel("端口:"))
        self.combo_port = QComboBox()
        self.combo_port.setMinimumWidth(120)
        self.combo_port.addItems([p.device for p in serial.tools.list_ports.comports()])
        serial_layout.addWidget(self.combo_port)

        serial_layout.addWidget(QLabel("波特率:"))
        self.combo_baud = QComboBox()
        self.combo_baud.addItems(["9600", "115200", "230400", "460800"])
        self.combo_baud.setCurrentText("115200")
        serial_layout.addWidget(self.combo_baud)

        btn_refresh = QPushButton("刷新")
        btn_refresh.clicked.connect(lambda: self.combo_port.clear() or
                                    self.combo_port.addItems([p.device for p in serial.tools.list_ports.comports()]))
        serial_layout.addWidget(btn_refresh)

        self.btn_serial_toggle = QPushButton("打开串口")
        self.btn_serial_toggle.setCheckable(True)
        self.btn_serial_toggle.clicked.connect(self._toggle_serial)
        serial_layout.addWidget(self.btn_serial_toggle)

        serial_layout.addStretch()
        serial_group.setLayout(serial_layout)
        main_layout.addWidget(serial_group)

        # ---- 固件与安全设置 ----
        file_group = QGroupBox("固件与安全配置")
        file_layout = QVBoxLayout()

        row1 = QHBoxLayout()
        row1.addWidget(QLabel("固件文件:"))
        self.edit_file = QLineEdit()
        self.edit_file.setPlaceholderText("选择或拖拽 .bin 文件...")
        # self.edit_file.setAcceptDrops(True)
        btn_browse = QPushButton("浏览...")
        btn_browse.clicked.connect(lambda: self.edit_file.setText(QFileDialog.getOpenFileName()[0]))
        row1.addWidget(self.edit_file)
        row1.addWidget(btn_browse)
        file_layout.addLayout(row1)

        row2 = QHBoxLayout()
        row2.addWidget(QLabel("固件版本:"))
        self.edit_version = QSpinBox()
        self.edit_version.setRange(0, 9999)
        self.edit_version.setValue(1)
        row2.addWidget(self.edit_version)
        row2.addStretch()
        row2.addWidget(QLabel("设备UID:"))
        self.edit_uid = QLineEdit()
        self.edit_uid.setPlaceholderText("24位十六进制")
        btn_capture = QPushButton("自动获取")
        btn_capture.clicked.connect(self._capture_uid)
        row2.addWidget(self.edit_uid)
        row2.addWidget(btn_capture)
        file_layout.addLayout(row2)

        row3 = QHBoxLayout()
        row3.addWidget(QLabel("私钥(Hex):"))
        self.edit_key = QLineEdit()
        self.edit_key.setEchoMode(QLineEdit.Password)
        self.edit_key.setPlaceholderText("64位十六进制私钥")
        self.chk_show_key = QCheckBox("显示私钥")
        self.chk_show_key.stateChanged.connect(self._toggle_key_visibility)
        row3.addWidget(self.edit_key)
        row3.addWidget(self.chk_show_key)
        file_layout.addLayout(row3)

        file_group.setLayout(file_layout)
        main_layout.addWidget(file_group)

        # ---- 控制按钮 ----
        btn_layout = QHBoxLayout()
        self.btn_start = QPushButton("开始升级")
        self.btn_start.setMinimumHeight(36)
        self.btn_start.clicked.connect(self._start_ota)
        self.btn_stop = QPushButton("停止")
        self.btn_stop.setMinimumHeight(36)
        self.btn_stop.setEnabled(False)
        self.btn_stop.clicked.connect(self._stop_ota)
        btn_layout.addWidget(self.btn_start)
        btn_layout.addWidget(self.btn_stop)
        main_layout.addLayout(btn_layout)

        # ---- 进度条 ----
        self.progress = QProgressBar()
        self.progress.setFormat("%p%")
        main_layout.addWidget(self.progress)

        # ---- 日志区域 ----
        log_group = QGroupBox("升级日志")
        log_layout = QVBoxLayout()
        self.log_edit = QTextEdit()
        self.log_edit.setReadOnly(True)
        log_layout.addWidget(self.log_edit)
        log_group.setLayout(log_layout)
        main_layout.addWidget(log_group)

        self.setAcceptDrops(True)                # 主窗口自身可接受拖放

    # ---------- 拖拽文件支持 ----------
    def dragEnterEvent(self, event: QDragEnterEvent):
        if event.mimeData().hasUrls():
            event.acceptProposedAction()
    def dragMoveEvent(self, event):
        event.acceptProposedAction()

    def dropEvent(self, event: QDropEvent):
        for url in event.mimeData().urls():
            path = url.toLocalFile()
            if path.lower().endswith('.bin'):
                self.edit_file.setText(path)
                break

    # ---------- 配置存取 ----------
    def _load_config(self):
        self.edit_file.setText(self.config.get("last_file", ""))
        self.edit_version.setValue(int(self.config.get("last_version", 1)))
        self.edit_uid.setText(self.config.get("last_uid", ""))
        self.edit_key.setText(self.config.get("last_key", ""))
        # 私钥不自动填充

    def _save_config(self):
        self.config.set("last_port", self.combo_port.currentText())
        self.config.set("last_baudrate", self.combo_baud.currentText())
        self.config.set("last_file", self.edit_file.text())
        self.config.set("last_version", str(self.edit_version.value()))
        self.config.set("last_uid", self.edit_uid.text())
        self.config.set("last_key", self.edit_key.text())

    # ---------- 串口控制 ----------
    def _toggle_serial(self):
        if not self.serial_opened:
            port = self.combo_port.currentText()
            baud = int(self.combo_baud.currentText())
            try:
                self.serial_instance = serial.Serial(port, baud, timeout=0.5)
                self.serial_opened = True
                self.btn_serial_toggle.setText("关闭串口")
                self.combo_port.setEnabled(False)
                self.combo_baud.setEnabled(False)
                self._append_log(f"串口 {port} 已打开", COLOR_OK)
            except Exception as e:
                QMessageBox.critical(self, "串口错误", f"无法打开串口: {str(e)}")
        else:
            if self.serial_instance and self.serial_instance.is_open:
                self.serial_instance.close()
            self.serial_opened = False
            self.serial_instance = None
            self.btn_serial_toggle.setText("打开串口")
            self.combo_port.setEnabled(True)
            self.combo_baud.setEnabled(True)
            self._append_log("串口已关闭", COLOR_DEBUG)

    def _auto_close_serial(self):
        if self.serial_opened:
            self._toggle_serial()

    # ---------- UID 自动获取 ----------
    def _capture_uid(self):
        if not self.serial_opened:
            QMessageBox.warning(self, "提示", "请先打开串口")
            return
        self.uid_thread = UidCaptureThread(self.serial_instance)
        self.uid_thread.uid_captured.connect(self.edit_uid.setText)
        self.uid_thread.log_signal.connect(self._append_log)
        self.uid_thread.start()

    # ---------- 升级控制 ----------
    def _start_ota(self):
        if not self.serial_opened:
            QMessageBox.warning(self, "提示", "请先打开串口")
            return

        file_path = self.edit_file.text()
        version = self.edit_version.value()
        uid = self.edit_uid.text()
        key = self.edit_key.text()

        if not all([file_path, uid, key]):
            QMessageBox.warning(self, "参数缺失", "请填写所有必要参数")
            return

        reply = QMessageBox.question(
            self, "确认升级",
            f"固件: {os.path.basename(file_path)}\n"
            f"版本: {version}\n"
            f"UID: {uid[:8]}...\n\n"
            "确认开始升级？",
            QMessageBox.Yes | QMessageBox.No
        )
        if reply != QMessageBox.Yes:
            return

        self._save_config()
        self.btn_start.setEnabled(False)
        self.btn_stop.setEnabled(True)
        self.progress.setValue(0)
        self.log_edit.clear()

        self.worker = OtaWorker(
            serial_instance=self.serial_instance,
            file_path=file_path,
            version=version,
            uid_hex=uid,
            private_key_hex=key
        )
        self.worker.log_signal.connect(self._append_log)
        self.worker.progress_signal.connect(self.progress.setValue)
        self.worker.finished_signal.connect(self._on_finished)
        self.worker.start()

    def _stop_ota(self):
        if self.worker and self.worker.isRunning():
            self.worker.stop()
            self.btn_start.setEnabled(True)
            self.btn_stop.setEnabled(False)
            self._append_log("用户手动停止", COLOR_WARN)

    def _on_finished(self, success):
        self.btn_start.setEnabled(True)
        self.btn_stop.setEnabled(False)
        if success:
            QMessageBox.information(self, "完成", "固件升级成功！")
            self._auto_close_serial()
        else:
            QMessageBox.critical(self, "失败", "升级失败，请查看日志")

    def _append_log(self, message, color=COLOR_DEFAULT):
        self.log_edit.append(f'<span style="color:#000000;">{message}</span>')

    def _toggle_key_visibility(self, state):
        if state == Qt.Checked:
            self.edit_key.setEchoMode(QLineEdit.Normal)
        else:
            self.edit_key.setEchoMode(QLineEdit.Password)

    def closeEvent(self, event):
        if self.serial_instance and self.serial_instance.is_open:
            self.serial_instance.close()
        event.accept()

    def eventFilter(self, obj, event):
        from PyQt5.QtCore import QEvent
        if obj == self.edit_file:
            if event.type() == QEvent.DragEnter:
                event.acceptProposedAction()
                return True
            elif event.type() == QEvent.Drop:
                mime = event.mimeData()
                if mime.hasUrls():
                    path = mime.urls()[0].toLocalFile()
                    if path.lower().endswith('.bin'):
                        self.edit_file.setText(path)
                        return True
                return True
        return super().eventFilter(obj, event)