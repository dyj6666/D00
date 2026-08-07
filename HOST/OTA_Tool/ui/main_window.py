import sys
import os
import time
import serial
from PyQt5.QtWidgets import (QMainWindow, QWidget, QFrame, QVBoxLayout, QHBoxLayout,
                             QLabel, QLineEdit, QComboBox, QPushButton,
                             QTextEdit, QProgressBar, QFileDialog, QMessageBox,
                             QGroupBox, QFormLayout, QSpinBox, QCheckBox)
from PyQt5.QtCore import Qt
from PyQt5.QtGui import QDragEnterEvent, QDropEvent, QFont
import serial.tools.list_ports
from core.ota_worker import OtaWorker
from core.version_lib import load_lib
from ui.uid_capture_thread import UidCaptureThread
from utils.config import Config

VERSION = "v2.1.0"

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
        self.resize(900, 760)
        self.setMinimumSize(820, 660)
        self.config = Config()

        # 应用样式
        style_path = os.path.join(os.path.dirname(__file__), "styles.qss")
        if os.path.exists(style_path):
            with open(style_path, 'r', encoding='utf-8') as f:
                self.setStyleSheet(f.read())

        self.worker = None
        self.lib_data = None
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
        self.combo_baud.addItems(["9600", "115200", "230400", "460800", "921600"])
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


        row0 = QHBoxLayout()
        row0.addWidget(QLabel("版本库:"))
        self.combo_lib = QComboBox()
        self.combo_lib.setMinimumWidth(280)
        self.combo_lib.currentIndexChanged.connect(self._on_lib_selected)
        btn_lib_refresh = QPushButton("刷新")
        btn_lib_refresh.clicked.connect(self._reload_lib)
        row0.addWidget(self.combo_lib)
        row0.addWidget(btn_lib_refresh)
        row0.addStretch()
        file_layout.addLayout(row0)
        self._reload_lib()
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
        btn_layout.addWidget(QLabel("升级模式:"))
        self.combo_mode = QComboBox()
        self.combo_mode.addItems(["HOSTLINK 运行时", "YMODEM 传统(BOOT)"])
        self.combo_mode.setCurrentIndex(0)
        self.combo_mode.currentIndexChanged.connect(self._on_mode_changed)
        btn_layout.addWidget(self.combo_mode)
        btn_layout.addStretch()
        self.btn_start = QPushButton("开始升级")
        self.btn_start.setObjectName("primary")
        self.btn_start.setMinimumHeight(42)
        self.btn_start.clicked.connect(self._start_ota)
        self.btn_stop = QPushButton("停止")
        self.btn_stop.setMinimumHeight(36)
        self.btn_stop.setEnabled(False)
        self.btn_stop.clicked.connect(self._stop_ota)
        btn_layout.addWidget(self.btn_start)
        btn_layout.addWidget(self.btn_stop)
        main_layout.addLayout(btn_layout)
        batch_row = QHBoxLayout()
        batch_row.addWidget(QLabel("批量端口(逗号分隔):"))
        self.edit_batch_ports = QLineEdit()
        self.edit_batch_ports.setPlaceholderText("COM13,COM16,...")
        batch_row.addWidget(self.edit_batch_ports)
        self.btn_batch = QPushButton("批量升级")
        self.btn_batch.setObjectName("batch")
        self.btn_batch.clicked.connect(self._start_batch)
        batch_row.addWidget(self.btn_batch)
        batch_row.addStretch()
        main_layout.addLayout(batch_row)


        self.lbl_mode_hint = QLabel()
        self.lbl_mode_hint.setStyleSheet("color: #2F4F4F;")
        main_layout.addWidget(self.lbl_mode_hint)
        self._on_mode_changed(0)


        # ---- 升级阶段流程条 ----
        self.stage_bar = QFrame()
        self.stage_bar.setObjectName("stage_bar")
        self.stage_layout = QHBoxLayout(self.stage_bar)
        self.stage_layout.setContentsMargins(0, 0, 0, 0)
        self.stage_layout.setSpacing(6)
        self.stage_segments = []
        for name in ["IDLE", "DOWNLOADING", "VERIFYING", "COMMITTED", "RUNNING"]:
            lbl = QLabel(name)
            lbl.setAlignment(Qt.AlignCenter)
            self.stage_layout.addWidget(lbl, 1)
            self.stage_segments.append((name, lbl))
        main_layout.addWidget(self.stage_bar)
        self._set_stage("IDLE")
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
        main_layout.addWidget(log_group, 1)

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
            private_key_hex=key,
            mode="ymodem" if self.combo_mode.currentIndex() == 1 else "hostlink"
        )
        self.worker.log_signal.connect(self._append_log)
        self.worker.progress_signal.connect(self.progress.setValue)
        self.worker.finished_signal.connect(self._on_finished)
        self.worker.stage_signal.connect(self._set_stage)
        self.worker.start()

    def _on_mode_changed(self, index):
        if index == 0:
            self.lbl_mode_hint.setText(
                "HOSTLINK 运行时：APP 在线下载到 Download 区，BOOT 校验后切换；"
                "请使用数据口并选择 921600 波特率")
            if "921600" not in [self.combo_baud.itemText(i)
                                for i in range(self.combo_baud.count())]:
                self.combo_baud.addItem("921600")
        else:
            self.lbl_mode_hint.setText(
                "YMODEM 传统：需先让设备进入 BOOT 升级模式（发 ota 命令复位），"
                "使用升级口 115200 波特率")

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
        self.log_edit.append(f'<span style="color:{color};">{message}</span>')
        sb = self.log_edit.verticalScrollBar()
        sb.setValue(sb.maximum())

    def _toggle_key_visibility(self, state):
        if state == Qt.Checked:
            self.edit_key.setEchoMode(QLineEdit.Normal)
        else:
            self.edit_key.setEchoMode(QLineEdit.Password)


    # ---------- 升级阶段流程条 ----------
    def _set_stage(self, stage):
        segs = ["IDLE", "DOWNLOADING", "VERIFYING", "COMMITTED", "RUNNING"]
        idx = segs.index(stage) if stage in segs else 0
        done_ss = ("background:#27ae60;color:white;border-radius:6px;"
                   "padding:8px 4px;font-weight:bold;")
        cur_ss = ("background:#3498db;color:white;border-radius:6px;"
                  "padding:8px 4px;font-weight:bold;")
        wait_ss = ("background:#ecf0f1;color:#7f8c8d;border-radius:6px;"
                   "padding:8px 4px;font-weight:bold;")
        for i, (name, lbl) in enumerate(self.stage_segments):
            if i < idx:
                lbl.setStyleSheet(done_ss)
            elif i == idx:
                lbl.setStyleSheet(cur_ss)
            else:
                lbl.setStyleSheet(wait_ss)

    # ---------- 版本库 ----------
    def _reload_lib(self):
        self.lib_data = load_lib()
        self.combo_lib.blockSignals(True)
        self.combo_lib.clear()
        self.combo_lib.addItem("— 手动选择 —")
        for e in self.lib_data.get("entries", []):
            label = f"v{e.get('version','?')} (build {e.get('build','?')}) {e.get('note','')}"
            self.combo_lib.addItem(label, e)
        self.combo_lib.blockSignals(False)

    def _on_lib_selected(self, idx):
        if idx <= 0:
            return
        e = self.combo_lib.itemData(idx)
        if e:
            self.edit_file.setText(e.get("file", ""))
            self.edit_version.setValue(int(e.get("version", 1)))

    # ---------- 批量升级（HOSTLINK 多端口并发） ----------
    def _start_batch(self):
        ports = [p.strip() for p in self.edit_batch_ports.text().split(",") if p.strip()]
        if not ports:
            QMessageBox.warning(self, "提示", "请填写批量端口（逗号分隔）")
            return
        file_path = self.edit_file.text()
        version = self.edit_version.value()
        uid = self.edit_uid.text()
        key = self.edit_key.text()
        if not all([file_path, uid, key]):
            QMessageBox.warning(self, "参数缺失", "请填写所有必要参数")
            return
        baud = int(self.combo_baud.currentText())
        self.progress.setValue(0)
        for port in ports:
            try:
                ser = serial.Serial(port, baud, timeout=0.5)
            except Exception as ex:
                self._append_log(f"[{port}] 打开失败: {ex}", COLOR_ERROR)
                continue
            w = OtaWorker(serial_instance=ser, file_path=file_path, version=version,
                          uid_hex=uid, private_key_hex=key, mode="hostlink")
            w.log_signal.connect(lambda m, c, p=port: self._append_log(f"[{p}] {m}", c))
            w.progress_signal.connect(self.progress.setValue)
            w.stage_signal.connect(self._set_stage)
            w.finished_signal.connect(lambda ok, p=port: self._batch_done(ok, p))
            w.start()
            self._append_log(f"[{port}] 批量升级已启动", COLOR_INFO)

    def _batch_done(self, ok, port):
        self._append_log(f"[{port}] 升级{'成功' if ok else '失败'}",
                         COLOR_OK if ok else COLOR_ERROR)

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
