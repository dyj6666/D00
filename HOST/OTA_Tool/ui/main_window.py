"""D00 OTA 升级中心：UART / TCP / HTTP 三通道统一上位机。

界面分区：模式选择 -> 传输设置 -> 固件与安全 -> 实时仪表盘 -> 日志/版本库。
"""

from __future__ import annotations

import os
import time

from PyQt5.QtCore import Qt
from PyQt5.QtGui import QDragEnterEvent, QDropEvent
from PyQt5.QtWidgets import (QCheckBox, QComboBox, QFileDialog, QFrame,
                             QFormLayout, QGridLayout, QHBoxLayout, QLabel,
                             QLineEdit, QMainWindow, QMessageBox,
                             QProgressBar, QPushButton, QSpinBox,
                             QSplitter, QStackedWidget, QTextEdit,
                             QVBoxLayout, QWidget)

from core.ota_engine import OtaEngine
from core.transport import TcpTransport, TransportError, UartTransport
from core.version_lib import load_lib
from utils.config import Config

VERSION = "v3.0.0"

# 日志配色（暗色主题）
_C = {
    "default": "#cbd5e1",
    "cyan": "#67e8f9",
    "green": "#34d399",
    "yellow": "#fbbf24",
    "orange": "#fb923c",
    "red": "#f87171",
    "gray": "#94a3b8",
}


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"D00 OTA 升级中心 {VERSION} —— 董衍俊")
        self.resize(1320, 880)
        self.setMinimumSize(1160, 780)
        self.config = Config()
        self.engine = None
        self._batch_engines = []
        self._batch_running = False
        self._t0 = 0.0

        style_path = os.path.join(os.path.dirname(__file__), "styles.qss")
        if os.path.exists(style_path):
            with open(style_path, "r", encoding="utf-8") as f:
                self.setStyleSheet(f.read())

        self._init_ui()
        self._load_config()
        self.installEventFilter(self)

    # ================================================================
    # UI 构建
    # ================================================================
    def _init_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setSpacing(12)
        root.setContentsMargins(18, 14, 18, 14)

        root.addWidget(self._build_header())

        # 左右分栏：左=配置与操作，右=仪表盘/日志/版本库（互不挤压）
        splitter = QSplitter(Qt.Horizontal)
        splitter.setChildrenCollapsible(False)
        splitter.addWidget(self._build_left_panel())
        splitter.addWidget(self._build_right_panel())
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        splitter.setSizes([460, 820])
        root.addWidget(splitter, 1)

        self.statusBar().showMessage("就绪 · 选择通信模式并配置固件")

    def _build_left_panel(self) -> QWidget:
        """左栏：模式选择 + 传输设置 + 固件与安全 + 操作按钮。"""
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(12)
        lay.addWidget(self._build_mode_selector())
        lay.addWidget(self._build_transport_card())
        lay.addWidget(self._build_firmware_card())
        lay.addWidget(self._build_actions())
        lay.addStretch()
        w.setMinimumWidth(460)
        return w

    def _build_right_panel(self) -> QWidget:
        """右栏：仪表盘 + 日志 + 版本库。"""
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(12)
        lay.addWidget(self._build_dashboard())
        lay.addWidget(self._build_log_card(), 1)
        lay.addLayout(self._build_version_row())
        return w

    def _build_header(self) -> QWidget:
        w = QWidget()
        lay = QHBoxLayout(w)
        lay.setContentsMargins(0, 0, 0, 0)
        title_box = QVBoxLayout()
        title = QLabel("D00 OTA 升级中心")
        title.setObjectName("app_title")
        sub = QLabel("UART · TCP · HTTP 三通道安全升级")
        sub.setObjectName("app_subtitle")
        title_box.addWidget(title)
        title_box.addWidget(sub)
        lay.addLayout(title_box, 1)
        self.lbl_dev = QLabel("设备: --")
        self.lbl_dev.setObjectName("device_pill")
        lay.addWidget(self.lbl_dev)
        return w

    def _build_mode_selector(self) -> QWidget:
        w = QWidget()
        lay = QHBoxLayout(w)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(10)
        self.mode_buttons = {}
        for key, label in (("uart", "🔌 UART (HOSTLINK)"),
                           ("tcp", "🌐 TCP :9020"),
                           ("http", "☁ HTTP 拉取")):
            b = QPushButton(label)
            b.setObjectName("mode_btn")
            b.setCheckable(True)
            b.setMinimumHeight(42)
            b.clicked.connect(lambda _, k=key: self._switch_mode(k))
            lay.addWidget(b, 1)
            self.mode_buttons[key] = b
        self.mode_buttons["uart"].setChecked(True)
        return w

    def _build_transport_card(self) -> QWidget:
        card = QFrame()
        card.setObjectName("card")
        lay = QVBoxLayout(card)
        lay.setContentsMargins(20, 16, 20, 16)
        lay.setSpacing(10)

        title = QLabel("传输设置")
        title.setObjectName("card_title")
        lay.addWidget(title)

        self.stack_transport = QStackedWidget()
        self.stack_transport.addWidget(self._build_uart_page())
        self.stack_transport.addWidget(self._build_tcp_page())
        self.stack_transport.addWidget(self._build_http_page())
        lay.addWidget(self.stack_transport)
        return card

    def _build_uart_page(self) -> QWidget:
        w = QWidget()
        g = QGridLayout(w)
        g.setContentsMargins(0, 2, 0, 2)
        g.setHorizontalSpacing(10)
        g.setVerticalSpacing(10)
        g.setColumnMinimumWidth(0, 112)

        g.addWidget(QLabel("OTA 端口:"), 0, 0)
        self.combo_port = QComboBox()
        self.combo_port.setMinimumWidth(150)
        g.addWidget(self.combo_port, 0, 1)
        self.btn_refresh = QPushButton("刷新")
        self.btn_refresh.setObjectName("ghost")
        self.btn_refresh.clicked.connect(self._refresh_ports)
        g.addWidget(self.btn_refresh, 0, 2)
        g.setColumnStretch(3, 1)

        g.addWidget(QLabel("波特率:"), 1, 0)
        self.combo_baud = QComboBox()
        self.combo_baud.addItems(
            ["115200", "230400", "460800", "921600", "1500000", "2000000"])
        self.combo_baud.setCurrentText("921600")
        self.combo_baud.setMinimumWidth(152)
        g.addWidget(self.combo_baud, 1, 1)
        self.btn_uart_test = QPushButton("测试连接")
        self.btn_uart_test.setObjectName("ghost")
        self.btn_uart_test.clicked.connect(self._test_uart)
        g.addWidget(self.btn_uart_test, 1, 2)

        self.chk_ymodem = QCheckBox("传统 YMODEM(BOOT)")
        g.addWidget(self.chk_ymodem, 2, 0, 1, 2)
        self.chk_verify_log = QCheckBox("验证启动日志")
        self.chk_verify_log.setChecked(True)
        g.addWidget(self.chk_verify_log, 2, 2)
        g.setRowStretch(3, 1)
        self._refresh_ports()
        return w

    def _build_tcp_page(self) -> QWidget:
        w = QWidget()
        g = QGridLayout(w)
        g.setContentsMargins(0, 2, 0, 2)
        g.setHorizontalSpacing(10)
        g.setVerticalSpacing(10)
        g.setColumnMinimumWidth(0, 112)

        g.addWidget(QLabel("设备 IP:"), 0, 0)
        self.edit_tcp_ip = QLineEdit("192.168.10.10")
        self.edit_tcp_ip.setMinimumWidth(170)
        g.addWidget(self.edit_tcp_ip, 0, 1)
        g.setColumnStretch(2, 1)

        g.addWidget(QLabel("端口:"), 1, 0)
        self.spin_tcp_port = QSpinBox()
        self.spin_tcp_port.setRange(1, 65535)
        self.spin_tcp_port.setValue(9020)
        self.spin_tcp_port.setMinimumWidth(100)
        g.addWidget(self.spin_tcp_port, 1, 1)
        self.btn_tcp_test = QPushButton("测试连接")
        self.btn_tcp_test.setObjectName("ghost")
        self.btn_tcp_test.clicked.connect(self._test_tcp)
        g.addWidget(self.btn_tcp_test, 1, 2)

        self.chk_no_resume = QCheckBox("从零开始(清会话)")
        g.addWidget(self.chk_no_resume, 2, 0, 1, 2)
        self.chk_verify_http = QCheckBox("状态页验证")
        self.chk_verify_http.setChecked(True)
        g.addWidget(self.chk_verify_http, 2, 2)
        g.setRowStretch(3, 1)
        return w

    def _build_http_page(self) -> QWidget:
        w = QWidget()
        g = QGridLayout(w)
        g.setContentsMargins(0, 2, 0, 2)
        g.setHorizontalSpacing(10)
        g.setVerticalSpacing(10)
        g.setColumnMinimumWidth(0, 128)

        g.addWidget(QLabel("服务端口:"), 0, 0)
        self.spin_http_port = QSpinBox()
        self.spin_http_port.setRange(1024, 65535)
        self.spin_http_port.setValue(8080)
        self.spin_http_port.setMinimumWidth(100)
        g.addWidget(self.spin_http_port, 0, 1)
        self.btn_http_test = QPushButton("测试连接")
        self.btn_http_test.setObjectName("ghost")
        self.btn_http_test.clicked.connect(self._test_http)
        g.addWidget(self.btn_http_test, 0, 2)
        g.setColumnStretch(3, 1)

        g.addWidget(QLabel("板端 IP:"), 1, 0)
        self.edit_board_ip = QLineEdit("192.168.10.10")
        self.edit_board_ip.setMinimumWidth(150)
        g.addWidget(self.edit_board_ip, 1, 1)

        g.addWidget(QLabel("控制通道:"), 2, 0)
        self.combo_ctl = QComboBox()
        self.combo_ctl.addItems(["UART (COM9)", "TCP :9000"])
        self.combo_ctl.setMinimumWidth(130)
        g.addWidget(self.combo_ctl, 2, 1)

        g.addWidget(QLabel("通道端口/IP:"), 3, 0)
        self.edit_ctl = QLineEdit("COM9")
        self.edit_ctl.setMinimumWidth(150)
        g.addWidget(self.edit_ctl, 3, 1)
        self.chk_http_verify = QCheckBox("状态页验证")
        self.chk_http_verify.setChecked(True)
        g.addWidget(self.chk_http_verify, 4, 0, 1, 2)
        g.setRowStretch(5, 1)
        return w

    def _build_firmware_card(self) -> QWidget:
        card = QFrame()
        card.setObjectName("card")
        lay = QVBoxLayout(card)
        lay.setContentsMargins(16, 14, 16, 14)
        lay.setSpacing(8)
        lay.addWidget(QLabel("固件与安全", objectName="card_title"))

        form = QFormLayout()
        form.setHorizontalSpacing(12)
        form.setVerticalSpacing(10)
        form.setLabelAlignment(Qt.AlignLeft | Qt.AlignVCenter)
        form.setFieldGrowthPolicy(QFormLayout.AllNonFixedFieldsGrow)

        # 固件文件
        row_file = QHBoxLayout()
        row_file.setSpacing(8)
        self.edit_file = QLineEdit()
        self.edit_file.setPlaceholderText("选择或拖拽 .bin 文件到此处...")
        row_file.addWidget(self.edit_file, 1)
        btn = QPushButton("浏览")
        btn.setObjectName("ghost")
        btn.clicked.connect(self._browse_file)
        row_file.addWidget(btn)
        lbl_file = QLabel("固件文件:")
        lbl_file.setMinimumWidth(120)
        form.addRow(lbl_file, row_file)

        # 版本
        lbl_ver = QLabel("版本:")
        lbl_ver.setMinimumWidth(120)
        self.spin_version = QSpinBox()
        self.spin_version.setRange(0, 99999)
        self.spin_version.setValue(1)
        self.spin_version.setMinimumWidth(92)
        form.addRow(lbl_ver, self.spin_version)

        # 构建号
        lbl_bld = QLabel("构建号:")
        lbl_bld.setMinimumWidth(120)
        self.lbl_build = QLabel("自动")
        self.lbl_build.setObjectName("build_badge")
        self.lbl_build.setAlignment(Qt.AlignCenter)
        form.addRow(lbl_bld, self.lbl_build)

        # 设备 UID
        row_uid = QHBoxLayout()
        row_uid.setSpacing(8)
        self.edit_uid = QLineEdit()
        self.edit_uid.setPlaceholderText("24位十六进制")
        row_uid.addWidget(self.edit_uid, 1)
        btn_uid = QPushButton("自动获取")
        btn_uid.setObjectName("ghost")
        btn_uid.clicked.connect(self._capture_uid)
        row_uid.addWidget(btn_uid)
        lbl_uid = QLabel("设备 UID:")
        lbl_uid.setMinimumWidth(120)
        form.addRow(lbl_uid, row_uid)

        # 私钥
        row_key = QHBoxLayout()
        row_key.setSpacing(8)
        self.edit_key = QLineEdit()
        self.edit_key.setEchoMode(QLineEdit.Password)
        self.edit_key.setPlaceholderText("64位十六进制私钥，或环境变量 OTA_PRIVKEY")
        row_key.addWidget(self.edit_key, 1)
        self.chk_key = QCheckBox("显示")
        self.chk_key.stateChanged.connect(
            lambda s: self.edit_key.setEchoMode(
                QLineEdit.Normal if s == Qt.Checked else QLineEdit.Password))
        row_key.addWidget(self.chk_key)
        lbl_key_l = QLabel("私钥(Hex):")
        lbl_key_l.setMinimumWidth(120)
        form.addRow(lbl_key_l, row_key)

        self.lbl_key = QLabel()
        self.lbl_key.setWordWrap(True)
        self.lbl_key.setText("未设置（粘贴或设 OTA_PRIVKEY）")
        form.addRow("", self.lbl_key)
        lay.addLayout(form)
        return card

    def _build_dashboard(self) -> QWidget:
        card = QFrame()
        card.setObjectName("card")
        lay = QVBoxLayout(card)
        lay.setContentsMargins(20, 16, 20, 16)
        lay.setSpacing(12)

        # 阶段流程条
        self.stage_bar = QWidget()
        self.stage_lay = QHBoxLayout(self.stage_bar)
        self.stage_lay.setContentsMargins(0, 0, 0, 0)
        self.stage_lay.setSpacing(6)
        self.stage_segs = []
        for name in ["空闲", "擦除", "下载", "校验", "提交", "运行"]:
            lbl = QLabel(name)
            lbl.setAlignment(Qt.AlignCenter)
            lbl.setObjectName("stage_seg")
            self.stage_lay.addWidget(lbl, 1)
            self.stage_segs.append(lbl)
        lay.addWidget(self.stage_bar)

        # 进度条
        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        self.progress.setValue(0)
        self.progress.setFormat("%p%")
        lay.addWidget(self.progress)

        # 统计行
        stats = QHBoxLayout()
        stats.setSpacing(10)
        self.lbl_stat = QLabel("进度  0 / 0 字节")
        self.lbl_speed = QLabel("速率  --")
        self.lbl_eta = QLabel("剩余  --")
        self.lbl_elapsed = QLabel("耗时  0.0s")
        for l in (self.lbl_stat, self.lbl_speed, self.lbl_eta, self.lbl_elapsed):
            l.setObjectName("stat")
            l.setMinimumWidth(200)
            stats.addWidget(l, 1)
        lay.addLayout(stats)
        return card

    def _build_actions(self) -> QWidget:
        box = QWidget()
        lay = QVBoxLayout(box)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(10)
        r1 = QHBoxLayout()
        r1.setSpacing(10)
        self.btn_start = QPushButton("🚀 开始升级")
        self.btn_start.setObjectName("primary")
        self.btn_start.setMinimumHeight(48)
        self.btn_start.clicked.connect(self._start_ota)
        r1.addWidget(self.btn_start, 1)
        self.btn_stop = QPushButton("⏹ 停止")
        self.btn_stop.setObjectName("danger")
        self.btn_stop.setMinimumHeight(44)
        self.btn_stop.setEnabled(False)
        self.btn_stop.clicked.connect(self._stop_ota)
        r1.addWidget(self.btn_stop)
        lay.addLayout(r1)
        r2 = QHBoxLayout()
        r2.setSpacing(10)
        lbl_batch = QLabel("批量端口(逗号分隔):")
        lbl_batch.setMinimumWidth(170)
        r2.addWidget(lbl_batch)
        self.edit_batch = QLineEdit()
        self.edit_batch.setPlaceholderText("COM13,COM16,...（仅 UART 模式）")
        r2.addWidget(self.edit_batch, 1)
        self.btn_batch = QPushButton("批量升级")
        self.btn_batch.setObjectName("batch")
        self.btn_batch.clicked.connect(self._start_batch)
        r2.addWidget(self.btn_batch)
        lay.addLayout(r2)
        return box

    def _build_log_card(self) -> QWidget:
        card = QFrame()
        card.setObjectName("card")
        lay = QVBoxLayout(card)
        lay.setContentsMargins(20, 16, 20, 16)
        lay.setSpacing(10)
        head = QHBoxLayout()
        head.addWidget(QLabel("升级日志", objectName="card_title"))
        head.addStretch()
        btn_clear = QPushButton("清空")
        btn_clear.setObjectName("ghost")
        btn_clear.clicked.connect(lambda: self.log_edit.clear())
        head.addWidget(btn_clear)
        btn_export = QPushButton("导出")
        btn_export.setObjectName("ghost")
        btn_export.clicked.connect(self._export_log)
        head.addWidget(btn_export)
        lay.addLayout(head)
        self.log_edit = QTextEdit()
        self.log_edit.setReadOnly(True)
        lay.addWidget(self.log_edit)
        return card

    def _build_version_row(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setSpacing(10)
        lbl_lib = QLabel("版本库:")
        lbl_lib.setMinimumWidth(72)
        row.addWidget(lbl_lib)
        self.combo_lib = QComboBox()
        self.combo_lib.setMinimumWidth(620)
        self.combo_lib.currentIndexChanged.connect(self._on_lib_selected)
        row.addWidget(self.combo_lib)
        btn = QPushButton("刷新")
        btn.setObjectName("ghost")
        btn.clicked.connect(self._reload_lib)
        row.addWidget(btn)
        row.addStretch()
        self.lbl_dev_info = QLabel("")
        self.lbl_dev_info.setObjectName("dev_info")
        row.addWidget(self.lbl_dev_info)
        return row

    # ================================================================
    # 模式与配置
    # ================================================================
    def _switch_mode(self, mode: str):
        for k, b in self.mode_buttons.items():
            b.setChecked(k == mode)
        self.stack_transport.setCurrentIndex(
            ["uart", "tcp", "http"].index(mode))
        self._refresh_ports()

    def _current_mode(self) -> str:
        for k, b in self.mode_buttons.items():
            if b.isChecked():
                return k
        return "uart"

    def _refresh_ports(self):
        ports = UartTransport.list_ports()
        cur = self.combo_port.currentText()
        self.combo_port.blockSignals(True)
        self.combo_port.clear()
        self.combo_port.addItems(ports)
        if cur in ports:
            self.combo_port.setCurrentText(cur)
        self.combo_port.blockSignals(False)

    def _load_config(self):
        self.edit_file.setText(self.config.get("last_file", ""))
        self.spin_version.setValue(int(self.config.get("last_version", 1)))
        self.edit_uid.setText(self.config.get("last_uid", ""))
        self.edit_tcp_ip.setText(self.config.get("last_tcp_ip", "192.168.10.10"))
        self.spin_tcp_port.setValue(int(self.config.get("last_tcp_port", 9020)))
        self.spin_http_port.setValue(int(self.config.get("last_http_port", 8080)))
        self.edit_ctl.setText(self.config.get("last_ctl", "COM9"))
        self.edit_board_ip.setText(self.config.get("last_board_ip", "192.168.10.10"))
        port = self.config.get("last_port", "")
        if port and self.combo_port.findText(port) >= 0:
            self.combo_port.setCurrentText(port)
        baud = self.config.get("last_baudrate", "921600")
        if self.combo_baud.findText(baud) >= 0:
            self.combo_baud.setCurrentText(baud)
        env_key = os.environ.get("OTA_PRIVKEY", "").strip()
        if env_key:
            self.edit_key.setText(env_key)
            self.lbl_key.setText("✓ 环境变量已注入")
            self.lbl_key.setStyleSheet("color:#34d399;")
        else:
            self.lbl_key.setText("未设置：粘贴或设 OTA_PRIVKEY")
            self.lbl_key.setStyleSheet("color:#fbbf24;")
        self._reload_lib()

    def _persist_current(self):
        self.config.set("last_tcp_ip", self.edit_tcp_ip.text().strip())
        self.config.set("last_tcp_port", str(self.spin_tcp_port.value()))
        self.config.set("last_http_port", str(self.spin_http_port.value()))
        self.config.set("last_ctl", self.edit_ctl.text().strip())
        self.config.set("last_board_ip", self.edit_board_ip.text().strip())

    def _save_config(self):
        self.config.set("last_port", self.combo_port.currentText())
        self.config.set("last_baudrate", self.combo_baud.currentText())
        self.config.set("last_file", self.edit_file.text())
        self.config.set("last_version", str(self.spin_version.value()))
        self.config.set("last_uid", self.edit_uid.text())
        self._persist_current()

    # ================================================================
    # 连接测试
    # ================================================================
    def _test_uart(self):
        u = UartTransport(self.combo_port.currentText(),
                          int(self.combo_baud.currentText()))
        try:
            u.open()
            u.drain()
            from core.hostlink import build_get_info, CMD_GET_INFO
            r = u.cmd(build_get_info(), CMD_GET_INFO, timeout=1.5, retries=2)
            if r:
                ver = int.from_bytes(r[5:9], "little")
                self._append_log(f"✅ 串口在线 · 协议版本 {ver}", "green")
                self.lbl_dev.setText("设备: 在线 (UART)")
            else:
                self._append_log("⚠️ 串口已开但无响应（确认 OTA 口与波特率）",
                                 "orange")
        except TransportError as e:
            self._append_log(f"❌ {e}", "red")
        finally:
            u.close()

    def _test_tcp(self):
        ip = self.edit_tcp_ip.text().strip()
        t = TcpTransport(ip, self.spin_tcp_port.value())
        try:
            t.open()
            r = t.cmd(4, b"", timeout=3)   # STATUS
            state = r[0] if r else "?"
            self._append_log(f"✅ TCP 在线 · OTA 状态 state={state}", "green")
            self.lbl_dev.setText(f"设备: 在线 ({ip})")
        except TransportError as e:
            self._append_log(f"❌ {e}", "red")
        finally:
            t.close()

    def _test_http(self):
        """HTTP 模式连接测试：①PC 端 HTTP 服务端口可用且正确选路；
        ②控制通道（COM9 串口 / :9000 TCP）能连上板子 shell。"""
        import socket
        import threading
        from core.transport import HttpOtaServer
        port = self.spin_http_port.value()
        board_ip = self.edit_board_ip.text().strip()
        ctl_mode = "uart" if self.combo_ctl.currentText().startswith("UART") else "tcp"
        ok = True

        # 停掉上一次测试遗留的服务（若还在 30 秒窗口内）
        self._stop_http_test_srv()

        # 1) HTTP 服务可用性（绑定端口 + 按板端 IP 选路）
        try:
            srv = HttpOtaServer("", port)
            url = srv.start(board_ip)
            self._http_test_srv = srv
            timer = threading.Timer(30.0, self._stop_http_test_srv)
            timer.daemon = True
            timer.start()
            self._append_log(
                f"✅ HTTP 服务已启动: {url}（30 秒后自动关闭，可立即用浏览器验证）",
                "green")
        except Exception as e:
            self._append_log(f"❌ HTTP 服务: {e}", "red")
            ok = False

        # 2) 控制通道在线性
        if ctl_mode == "uart":
            ctl_port = self.edit_ctl.text().strip()
            u = UartTransport(ctl_port, 115200)
            try:
                u.open()
                u.drain()
                u.write(b"\r")
                buf = b""
                deadline = time.time() + 3
                while time.time() < deadline and u.is_open:
                    n = u._ser.in_waiting if u.is_open else 0
                    if n:
                        buf += u.read(n)
                        if b">" in buf:
                            break
                    time.sleep(0.05)
                u.close()
                if buf:
                    self._append_log(f"✅ 控制通道 UART 在线: {ctl_port} (shell 有回显)",
                                     "green")
                else:
                    self._append_log(
                        f"⚠️ 控制通道 {ctl_port} 无回显（确认板子已启动、口未占用）",
                        "orange")
                    ok = False
            except Exception as e:
                self._append_log(f"❌ 控制通道: {e}", "red")
                ok = False
        else:
            ctl_ip = self.edit_ctl.text().strip()
            try:
                s = socket.create_connection((ctl_ip, 9000), timeout=3)
                s.settimeout(2)
                welcome = s.recv(256)
                s.sendall(b"help\r")
                resp = s.recv(256)
                s.close()
                if welcome or resp:
                    self._append_log(f"✅ 控制通道 TCP 在线: {ctl_ip}:9000", "green")
                else:
                    self._append_log(f"⚠️ 控制通道 {ctl_ip}:9000 无响应", "orange")
                    ok = False
            except Exception as e:
                self._append_log(f"❌ 控制通道: {e}", "red")
                ok = False

        if ok:
            self.lbl_dev.setText(f"设备: 在线 ({board_ip})")
            self._append_log("HTTP 通道检查全部通过，可开始升级", "cyan")

    def _stop_http_test_srv(self):
        """关闭测试用 HTTP 服务（幂等）。"""
        srv = getattr(self, "_http_test_srv", None)
        if srv is not None:
            try:
                srv.stop()
            except Exception:
                pass
            self._http_test_srv = None

    # ================================================================
    # 固件/UID
    # ================================================================
    def _browse_file(self):
        path, _ = QFileDialog.getOpenFileName(self, "选择固件", "",
                                              "固件文件 (*.bin)")
        if path:
            self.edit_file.setText(path)

    def _capture_uid(self):
        port = self.combo_port.currentText()
        baud = int(self.combo_baud.currentText())
        self._append_log(f"监听 {port} 等待设备 UID 上报...", "cyan")
        import threading
        threading.Thread(target=self._uid_worker, args=(port, baud),
                         daemon=True).start()

    def _uid_worker(self, port, baud):
        try:
            u = UartTransport(port, baud)
            u.open()
            try:
                deadline = time.time() + 6
                buf = b""
                while time.time() < deadline:
                    n = u._ser.in_waiting if u.is_open else 0
                    if n:
                        buf += u.read(n)
                        text = buf.decode("utf-8", "replace")
                        if "DEV_UID:" in text:
                            uid = text.split("DEV_UID:")[1].strip().split()[0]
                            if len(uid) == 24:
                                self.edit_uid.setText(uid)
                                self._append_log(f"✅ 已获取 UID: {uid}", "green")
                                return
                        buf = buf[-1024:]
                    time.sleep(0.01)
                self._append_log("⚠️ 未检测到 UID 上报", "orange")
            finally:
                u.close()
        except TransportError as e:
            self._append_log(f"❌ {e}", "red")

    # ================================================================
    # 升级控制
    # ================================================================
    def _build_cfg(self) -> dict:
        mode = self._current_mode()
        cfg = {
            "mode": mode,
            "file": self.edit_file.text().strip(),
            "version": self.spin_version.value(),
            "build_no": 0,
            "uid": self.edit_uid.text().strip(),
            "key": self.edit_key.text().strip(),
            "verify_http": False,
            "verify_boot_log": False,
        }
        if mode == "uart":
            cfg.update({
                "uart_port": self.combo_port.currentText(),
                "uart_baud": int(self.combo_baud.currentText()),
                "use_ymodem": self.chk_ymodem.isChecked(),
                "verify_boot_log": self.chk_verify_log.isChecked(),
                "debug_port": "COM9",
                "debug_baud": 115200,
            })
        elif mode == "tcp":
            cfg.update({
                "tcp_ip": self.edit_tcp_ip.text().strip(),
                "tcp_port": self.spin_tcp_port.value(),
                "no_resume": self.chk_no_resume.isChecked(),
                "verify_http": self.chk_verify_http.isChecked(),
            })
        elif mode == "http":
            ctl = self.combo_ctl.currentText()
            cfg.update({
                "http_port": self.spin_http_port.value(),
                "board_ip": self.edit_board_ip.text().strip(),
                "ctl_mode": "uart" if ctl.startswith("UART") else "tcp",
                "ctl_port": self.edit_ctl.text().strip(),
                "ctl_baud": 115200,
                "ctl_ip": self.edit_ctl.text().strip(),
                "verify_http": self.chk_http_verify.isChecked(),
            })
        return cfg

    def _start_ota(self):
        if self.engine and self.engine.isRunning():
            return
        cfg = self._build_cfg()
        mode = self._current_mode()
        if not cfg["file"] or not cfg["uid"] or not cfg["key"]:
            QMessageBox.warning(self, "参数缺失", "请填写固件文件、UID 与私钥")
            return
        if mode == "uart" and not cfg["uart_port"]:
            QMessageBox.warning(self, "参数缺失", "请选择 OTA 串口")
            return
        if mode == "tcp" and not cfg["tcp_ip"]:
            QMessageBox.warning(self, "参数缺失", "请填写设备 IP")
            return
        reply = QMessageBox.question(
            self, "确认升级",
            f"模式: {mode.upper()}\n固件: {os.path.basename(cfg['file'])}\n"
            f"版本: v{cfg['version']}\n设备 UID: {cfg['uid'][:8]}...\n\n"
            "确认开始升级？",
            QMessageBox.Yes | QMessageBox.No)
        if reply != QMessageBox.Yes:
            return
        self._save_config()
        self._reset_dashboard()
        self.btn_start.setEnabled(False)
        self.btn_stop.setEnabled(True)
        self._t0 = time.perf_counter()
        self._append_log("── 新会话开始 ──────────────────────────", "cyan")
        self.engine = OtaEngine(mode, cfg)
        self._hook_engine(self.engine)
        self.engine.start()

    def _hook_engine(self, eng: OtaEngine):
        eng.log.connect(self._append_log)
        eng.progress.connect(self._on_progress)
        eng.stage.connect(self._set_stage)
        eng.device_info.connect(self._on_device_info)
        eng.finished.connect(lambda ok, msg: self._on_finished(ok, msg))

    def _stop_ota(self):
        if self.engine and self.engine.isRunning():
            self.engine.stop()
            self._append_log("⏹ 正在停止...", "orange")

    def _on_progress(self, done, total, speed, eta):
        self.progress.setValue(int(done * 100 / total) if total else 0)
        self.lbl_stat.setText(f"{done:,} / {total:,} 字节")
        if speed > 0:
            self.lbl_speed.setText(f"速率 {speed / 1024:.1f} KB/s")
            self.lbl_eta.setText(f"剩余 {eta:.1f}s")
        self.lbl_elapsed.setText(f"耗时 {time.perf_counter() - self._t0:.1f}s")

    def _on_device_info(self, info: dict):
        if isinstance(info, dict) and info.get("ip"):
            self.lbl_dev_info.setText(
                f"设备: v{info.get('ver','?')} · {info.get('ip')} · "
                f"堆 {info.get('heap_free','?')}B")

    def _on_finished(self, ok, msg):
        self.btn_start.setEnabled(True)
        self.btn_stop.setEnabled(False)
        self.engine = None
        if ok:
            self._append_log(f"✅ 升级成功 · {msg}", "green")
        else:
            self._append_log(f"❌ 升级失败 · {msg}", "red")

    def _reset_dashboard(self):
        self.progress.setValue(0)
        self.lbl_stat.setText("0 / 0 字节")
        self.lbl_speed.setText("速率 --")
        self.lbl_eta.setText("剩余 --")
        self.lbl_elapsed.setText("耗时 0.0s")
        self._set_stage("IDLE")

    def _set_stage(self, stage: str):
        mapping = {"IDLE": 0, "ERASING": 1, "DOWNLOADING": 2,
                   "VERIFYING": 3, "COMMITTED": 4, "RUNNING": 5,
                   "DONE": 6, "FAIL": 6}
        idx = mapping.get(stage, 0)
        for i, lbl in enumerate(self.stage_segs):
            if stage == "FAIL":
                lbl.setProperty("state", "fail")
            elif i < idx:
                lbl.setProperty("state", "done")
            elif i == idx and idx < 6:
                lbl.setProperty("state", "cur")
            else:
                lbl.setProperty("state", "wait")
            lbl.style().unpolish(lbl)
            lbl.style().polish(lbl)

    # ================================================================
    # 日志 / 版本库 / 拖放
    # ================================================================
    def _append_log(self, message: str, color: str = "default"):
        c = _C.get(color, _C["default"])
        ts = time.strftime("%H:%M:%S")
        self.log_edit.append(
            f'<span style="color:#64748b;">[{ts}]</span> '
            f'<span style="color:{c};">{message}</span>')
        sb = self.log_edit.verticalScrollBar()
        sb.setValue(sb.maximum())

    def _export_log(self):
        path, _ = QFileDialog.getSaveFileName(self, "导出日志", "ota_log.txt",
                                              "文本文件 (*.txt)")
        if path:
            with open(path, "w", encoding="utf-8") as f:
                f.write(self.log_edit.toPlainText())
            self._append_log(f"日志已导出: {path}", "gray")

    def _reload_lib(self):
        self.lib_data = load_lib()
        self.combo_lib.blockSignals(True)
        self.combo_lib.clear()
        self.combo_lib.addItem("— 选择历史版本 —")
        for e in self.lib_data.get("entries", []):
            self.combo_lib.addItem(
                f"v{e.get('version','?')} (build {e.get('build','?')}) "
                f"{e.get('note','')}", e)
        self.combo_lib.blockSignals(False)

    def _on_lib_selected(self, idx):
        if idx <= 0:
            return
        e = self.combo_lib.itemData(idx)
        if e:
            self.edit_file.setText(e.get("file", ""))
            self.spin_version.setValue(int(e.get("version", 1)))

    # ---- 批量升级（UART 多端口并发） ----
    def _start_batch(self):
        if self._batch_running:
            QMessageBox.warning(self, "提示", "批量升级正在进行中")
            return
        ports = list(dict.fromkeys(
            p.strip() for p in self.edit_batch.text().split(",") if p.strip()))
        if not ports:
            QMessageBox.warning(self, "提示", "请填写批量端口（逗号分隔）")
            return
        file_path = self.edit_file.text().strip()
        uid = self.edit_uid.text().strip()
        key = self.edit_key.text().strip()
        if not all([file_path, uid, key]):
            QMessageBox.warning(self, "参数缺失", "请填写固件文件、UID 与私钥")
            return
        self._append_log(
            f"批量 {len(ports)} 台：同批次设备 UID 需一致（AES 密钥由 UID 派生）",
            "orange")
        self._batch_running = True
        self.btn_batch.setEnabled(False)
        self._batch_engines = []
        # 串行预分配 build_no（避免并发竞争导致版本库损坏/重复）
        from core.version_lib import alloc_build_no, load_lib
        lib = load_lib()
        assigned = [(p, alloc_build_no(lib)) for p in ports]
        for port, bn in assigned:
            cfg = self._build_cfg()
            cfg.update({"uart_port": port, "build_no": bn, "verify_boot_log": False})
            eng = OtaEngine("uart", cfg)
            eng.log.connect(
                lambda m, c, p=port: self._append_log(f"[{p}] {m}", c))
            eng.progress.connect(self._on_progress)
            eng.stage.connect(self._set_stage)
            eng.finished.connect(
                lambda ok, msg, p=port: self._batch_done(ok, msg, p))
            self._batch_engines.append(eng)
            self._append_log(f"[{port}] 批量升级已启动 (build {bn})", "cyan")
            eng.start()

    def _batch_done(self, ok, msg, port):
        self._append_log(f"[{port}] 升级{'成功' if ok else '失败'} · {msg}",
                         "green" if ok else "red")
        if all(not e.isRunning() for e in self._batch_engines):
            self._batch_running = False
            self.btn_batch.setEnabled(True)
            self._append_log("批量升级全部结束", "cyan")

    def dragEnterEvent(self, event: QDragEnterEvent):
        if event.mimeData().hasUrls():
            event.acceptProposedAction()

    def dropEvent(self, event: QDropEvent):
        for url in event.mimeData().urls():
            p = url.toLocalFile()
            if p.lower().endswith(".bin"):
                self.edit_file.setText(p)
                break

    def closeEvent(self, event):
        if self.engine and self.engine.isRunning():
            self.engine.stop()
            self.engine.wait(3000)
        event.accept()
