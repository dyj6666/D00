#!/usr/bin/env python3
"""D00 DAP 调试台 —— 顶级 CMSIS-DAP 可视化上位机。

集成 DAP 全功能：连接/芯片信息、健壮烧录（500kHz+IWDG 窗口+分块+
校验）、OTA 全 DAP 触发（预置包+升级标志+复位）、内存读写、核心/外设
寄存器、目标控制、擦除/转储/比对。

用法：python dap_gui.py
"""

import os
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time

from PyQt5.QtCore import Qt, QTimer, QObject, pyqtSignal
from PyQt5.QtGui import QFont, QColor
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QGridLayout,
    QLabel, QLineEdit, QPushButton, QComboBox, QCheckBox, QTabWidget,
    QProgressBar, QPlainTextEdit, QFileDialog, QMessageBox, QGroupBox,
    QTableWidget, QTableWidgetItem, QHeaderView, QSpinBox, QFrame,
    QDockWidget, QAbstractItemView)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dap_core as dc


class _UiBridge(QObject):
    """工作线程 → 主线程信号桥：所有 UI 更新经此路由，杜绝跨线程操作。"""
    task = pyqtSignal(object, object)


def re_asm(line):
    """OpenOCD 反汇编行判定：以 8 位 hex 地址开头。"""
    return bool(re.match(r"0x[0-9a-fA-F]{8}", line))


class LogHub:
    """线程安全日志：工作线程写入队列，GUI 定时器取走渲染。"""

    def __init__(self, box):
        self.box = box
        self.q = queue.Queue()

    def write(self, tag, msg):
        try:
            self.q.put((time.strftime("%H:%M:%S"), tag, msg))
        except Exception:
            pass

    def drain(self):
        colors = {"I": "#475569", "S": "#059669", "E": "#dc2626", "D": "#7c3aed"}
        while True:
            try:
                ts, tag, msg = self.q.get_nowait()
            except queue.Empty:
                break
            color = colors.get(tag, "#475569")
            self.box.appendHtml(
                '<span style="color:#94a3b8">%s</span> '
                '<span style="color:%s">[%s]</span> %s' %
                (ts, color, tag, msg.replace("<", "&lt;")))


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("D00 DAP 调试台")
        self.resize(1180, 780)
        self.session = None
        self.cancel_evt = threading.Event()
        self.op_thread = None
        self.bridge = _UiBridge()
        self.bridge.task.connect(self._on_bridge_task)
        self.hold_timer = QTimer(self)
        self.hold_timer.timeout.connect(self._hold_feed)

        self._build_ui()
        self.log = LogHub(self.log_box)
        self.ticker = QTimer(self)
        self.ticker.timeout.connect(self.log.drain)
        self.ticker.start(120)

    # ---------------- UI ----------------

    def _build_ui(self):
        central = QWidget()
        root = QVBoxLayout(central)
        root.setContentsMargins(22, 18, 22, 12)
        root.setSpacing(12)

        # 头部
        head = QHBoxLayout()
        title_col = QVBoxLayout()
        t = QLabel("D00 DAP 调试台")
        t.setObjectName("app_title")
        sub = QLabel("CMSIS-DAP 全功能可视化 · 烧录 / OTA / 内存 / 寄存器 / 目标控制")
        sub.setObjectName("app_subtitle")
        title_col.addWidget(t)
        title_col.addWidget(sub)
        head.addLayout(title_col)
        head.addStretch(1)
        self.status_pill = QLabel("未连接")
        self.status_pill.setObjectName("device_pill")
        head.addWidget(self.status_pill)
        root.addLayout(head)

        # 连接工具栏
        bar = QHBoxLayout()
        self.btn_connect = QPushButton("连接 DAP")
        self.btn_connect.setObjectName("btn_primary")
        self.btn_disconnect = QPushButton("断开")
        self.btn_refresh = QPushButton("刷新信息")
        self.info_label = QLabel("芯片: --")
        self.info_label.setObjectName("info_line")
        for b in (self.btn_connect, self.btn_disconnect, self.btn_refresh):
            bar.addWidget(b)
        bar.addSpacing(10)
        bar.addWidget(self.info_label)
        bar.addStretch(1)
        root.addLayout(bar)
        # 工具栏按钮信号（此前的连接遗漏导致点击无反应）
        self.btn_connect.clicked.connect(self._connect)
        self.btn_disconnect.clicked.connect(self._disconnect)
        self.btn_refresh.clicked.connect(self._refresh_info)

        # 标签页
        self.tabs = QTabWidget()
        self.tabs.addTab(self._tab_flash(), "烧录")
        self.tabs.addTab(self._tab_ota(), "OTA 助手")
        self.tabs.addTab(self._tab_debug(), "调试")
        self.tabs.addTab(self._tab_tasks(), "RTOS 任务")
        self.tabs.addTab(self._tab_mem(), "内存")
        self.tabs.addTab(self._tab_regs(), "寄存器")
        self.tabs.addTab(self._tab_target(), "目标控制")
        self.tabs.addTab(self._tab_tools(), "工具")
        root.addWidget(self.tabs, 1)

        self.setCentralWidget(central)

        # 日志停靠
        dock = QDockWidget("运行日志", self)
        self.log_box = QPlainTextEdit()
        self.log_box.setReadOnly(True)
        self.log_box.setMaximumBlockCount(4000)
        dock.setWidget(self.log_box)
        self.addDockWidget(Qt.BottomDockWidgetArea, dock)
        self.resizeDocks([dock], [190], Qt.Vertical)

        self.btn_disconnect.setEnabled(False)
        self.btn_refresh.setEnabled(False)
        self._set_ops_enabled(False)

    def _tab_flash(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        g = QGroupBox("固件烧录（500kHz · 分块 · IWDG 窗口 · 整包校验）")
        gl = QGridLayout(g)
        self.f_file = QLineEdit()
        self.f_file.setPlaceholderText("选择 .bin 文件（BOOT / APP / OTA 包）")
        b = QPushButton("浏览...")
        b.clicked.connect(self._pick_file)
        gl.addWidget(QLabel("文件"), 0, 0)
        gl.addWidget(self.f_file, 0, 1)
        gl.addWidget(b, 0, 2)

        self.f_addr_combo = QComboBox()
        for name, addr in (("BOOT 0x08000000", 0x08000000),
                           ("RUN(APP) 0x08010000", 0x08010000),
                           ("DOWNLOAD 0x080A0000", 0x080A0000),
                           ("PARAM 0x080E0000", 0x080E0000),
                           ("自定义", 0)):
            self.f_addr_combo.addItem(name, addr)
        self.f_addr = QLineEdit("080A0000")
        self.f_addr.setEnabled(False)
        self.f_addr_combo.currentIndexChanged.connect(self._addr_combo_changed)
        gl.addWidget(QLabel("目标地址"), 1, 0)
        gl.addWidget(self.f_addr_combo, 1, 1)
        gl.addWidget(self.f_addr, 1, 2)

        self.f_clock = QComboBox()
        self.f_clock.addItems(["500 kHz", "1000 kHz"])
        self.f_chunk = QComboBox()
        self.f_chunk.addItems(["32 KB", "48 KB", "64 KB"])
        self.f_chunk.setCurrentIndex(1)
        self.f_verify = QCheckBox("烧录后整包校验")
        self.f_verify.setChecked(True)
        gl.addWidget(QLabel("SWD 时钟"), 2, 0)
        gl.addWidget(self.f_clock, 2, 1)
        gl.addWidget(QLabel("分块大小"), 3, 0)
        gl.addWidget(self.f_chunk, 3, 1)
        gl.addWidget(self.f_verify, 3, 2)
        lay.addWidget(g)

        row = QHBoxLayout()
        self.btn_flash = QPushButton("开始烧录")
        self.btn_flash.setObjectName("btn_primary")
        self.btn_flash_both = QPushButton("BOOT+APP 一键")
        self.btn_cancel = QPushButton("取消")
        self.progress = QProgressBar()
        self.progress.setTextVisible(True)
        row.addWidget(self.btn_flash)
        row.addWidget(self.btn_flash_both)
        row.addWidget(self.btn_cancel)
        row.addWidget(self.progress, 1)
        lay.addLayout(row)
        self.flash_status = QLabel("就绪")
        lay.addWidget(self.flash_status)
        self.btn_flash.clicked.connect(self._run_flash)
        self.btn_flash_both.clicked.connect(self._run_flash_both)
        self.btn_cancel.clicked.connect(self._cancel_op)
        lay.addStretch(1)
        return w

    def _tab_ota(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        g = QGroupBox("OTA 全 DAP 驱动（无需串口）")
        gl = QGridLayout(g)
        self.o_file = QLineEdit()
        self.o_file.setPlaceholderText("选择加密签名 OTA 包（_ota_*_pkg.bin）")
        b = QPushButton("浏览...")
        b.clicked.connect(lambda: self._pick_file(self.o_file))
        gl.addWidget(QLabel("OTA 包"), 0, 0)
        gl.addWidget(self.o_file, 0, 1)
        gl.addWidget(b, 0, 2)
        self.o_addr = QLineEdit("080A0000")
        gl.addWidget(QLabel("下载地址"), 1, 0)
        gl.addWidget(self.o_addr, 1, 1)
        hint = QLabel("一键流程：分块写入下载区 → 校验 → 写 BKP 升级标志(0x5A5A) → "
                      "复位 → BOOT 自动应用新固件")
        hint.setObjectName("hint")
        gl.addWidget(hint, 2, 0, 1, 3)
        lay.addWidget(g)
        row = QHBoxLayout()
        self.btn_ota_all = QPushButton("预置 + 触发升级")
        self.btn_ota_all.setObjectName("btn_primary")
        self.btn_ota_stage = QPushButton("仅预置包")
        self.btn_ota_trig = QPushButton("仅触发升级")
        row.addWidget(self.btn_ota_all)
        row.addWidget(self.btn_ota_stage)
        row.addWidget(self.btn_ota_trig)
        row.addStretch(1)
        lay.addLayout(row)
        self.btn_ota_all.clicked.connect(lambda: self._run_ota("all"))
        self.btn_ota_stage.clicked.connect(lambda: self._run_ota("stage"))
        self.btn_ota_trig.clicked.connect(lambda: self._run_ota("trigger"))
        lay.addStretch(1)
        return w

    def _tab_debug(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        g = QGroupBox("断点")
        gl = QGridLayout(g)
        self.d_bp_addr = QLineEdit("08010000")
        b_set = QPushButton("设置断点")
        b_clr = QPushButton("清除断点")
        b_lst = QPushButton("列出断点")
        gl.addWidget(QLabel("地址(HEX)"), 0, 0)
        gl.addWidget(self.d_bp_addr, 0, 1)
        gl.addWidget(b_set, 0, 2)
        gl.addWidget(b_clr, 0, 3)
        gl.addWidget(b_lst, 0, 4)
        lay.addWidget(g)
        g2 = QGroupBox("执行控制")
        gl2 = QGridLayout(g2)
        b_halt = QPushButton("暂停")
        b_run = QPushButton("继续")
        b_step = QPushButton("单步")
        gl2.addWidget(b_halt, 0, 0)
        gl2.addWidget(b_run, 0, 1)
        gl2.addWidget(b_step, 0, 2)
        self.d_bp_info = QLabel("断点: 无")
        gl2.addWidget(self.d_bp_info, 0, 3, 1, 2)
        lay.addWidget(g2)
        g3 = QGroupBox("反汇编")
        gl3 = QGridLayout(g3)
        self.d_dis_addr = QLineEdit("08010000")
        self.d_dis_n = QSpinBox()
        self.d_dis_n.setRange(4, 64)
        self.d_dis_n.setValue(16)
        b_dis = QPushButton("反汇编")
        gl3.addWidget(QLabel("地址"), 0, 0)
        gl3.addWidget(self.d_dis_addr, 0, 1)
        gl3.addWidget(QLabel("条数"), 0, 2)
        gl3.addWidget(self.d_dis_n, 0, 3)
        gl3.addWidget(b_dis, 0, 4)
        self.d_dis = QPlainTextEdit()
        self.d_dis.setReadOnly(True)
        self.d_dis.setFont(QFont("Consolas", 10))
        gl3.addWidget(self.d_dis, 1, 0, 1, 5)
        lay.addWidget(g3, 1)
        g4 = QGroupBox("故障 / 复位诊断")
        gl4 = QGridLayout(g4)
        b_fault = QPushButton("读取故障与复位原因")
        self.d_fault = QPlainTextEdit()
        self.d_fault.setReadOnly(True)
        self.d_fault.setFont(QFont("Consolas", 10))
        self.d_fault.setMaximumHeight(110)
        gl4.addWidget(b_fault, 0, 0)
        gl4.addWidget(self.d_fault, 1, 0)
        lay.addWidget(g4)
        b_set.clicked.connect(self._bp_set)
        b_clr.clicked.connect(self._bp_clear)
        b_lst.clicked.connect(self._bp_list)
        b_halt.clicked.connect(self._target_halt)
        b_run.clicked.connect(self._target_resume)
        b_step.clicked.connect(self._debug_step)
        b_dis.clicked.connect(self._debug_disasm)
        b_fault.clicked.connect(self._debug_fault)
        return w

    def _tab_tasks(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        row = QHBoxLayout()
        b = QPushButton("刷新任务")
        b.setObjectName("btn_primary")
        self.tk_info = QLabel("")
        row.addWidget(b)
        row.addWidget(self.tk_info, 1)
        lay.addLayout(row)
        self.tk_table = QTableWidget(0, 5)
        self.tk_table.setHorizontalHeaderLabels(
            ["任务", "优先级", "状态", "栈已用(词)", "TCB"])
        self.tk_table.horizontalHeader().setSectionResizeMode(
            0, QHeaderView.Stretch)
        for i in (1, 2, 3, 4):
            self.tk_table.horizontalHeader().setSectionResizeMode(
                i, QHeaderView.ResizeToContents)
        self.tk_table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        lay.addWidget(self.tk_table, 1)
        b.clicked.connect(self._tasks_refresh)
        return w

    def _tab_mem(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        g = QGroupBox("内存读取 / 写入")
        gl = QGridLayout(g)
        self.m_addr = QLineEdit("080A0000")
        self.m_len = QSpinBox()
        self.m_len.setRange(1, 256)
        self.m_len.setValue(8)
        self.m_addr2 = QLineEdit("080A0000")
        self.m_val = QLineEdit("00000000")
        b_r = QPushButton("读取")
        b_w = QPushButton("写入 32 位")
        gl.addWidget(QLabel("地址"), 0, 0)
        gl.addWidget(self.m_addr, 0, 1)
        gl.addWidget(QLabel("字数"), 0, 2)
        gl.addWidget(self.m_len, 0, 3)
        gl.addWidget(b_r, 0, 4)
        gl.addWidget(QLabel("写地址"), 1, 0)
        gl.addWidget(self.m_addr2, 1, 1)
        gl.addWidget(QLabel("值(HEX)"), 1, 2)
        gl.addWidget(self.m_val, 1, 3)
        gl.addWidget(b_w, 1, 4)
        lay.addWidget(g)
        self.m_dump = QPlainTextEdit()
        self.m_dump.setReadOnly(True)
        self.m_dump.setFont(QFont("Consolas", 10))
        lay.addWidget(self.m_dump, 1)
        b_r.clicked.connect(self._mem_read)
        b_w.clicked.connect(self._mem_write)
        return w

    def _tab_regs(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        top = QHBoxLayout()
        b = QPushButton("刷新寄存器")
        b.setObjectName("btn_primary")
        b2 = QPushButton("读取设备/固件/崩溃信息")
        b3 = QPushButton("清除崩溃记录")
        top.addWidget(b)
        top.addWidget(b2)
        top.addWidget(b3)
        top.addStretch(1)
        lay.addLayout(top)
        split = QHBoxLayout()
        left = QVBoxLayout()
        self.reg_table = QTableWidget(0, 2)
        self.reg_table.setHorizontalHeaderLabels(["寄存器", "值"])
        self.reg_table.horizontalHeader().setSectionResizeMode(
            0, QHeaderView.ResizeToContents)
        self.reg_table.horizontalHeader().setSectionResizeMode(
            1, QHeaderView.Stretch)
        self.reg_table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        left.addWidget(self.reg_table, 1)
        split.addLayout(left, 1)
        right = QVBoxLayout()
        self.reg_extra = QPlainTextEdit()
        self.reg_extra.setReadOnly(True)
        self.reg_extra.setFont(QFont("Consolas", 10))
        right.addWidget(QLabel("设备 / 固件 / 崩溃记录"))
        right.addWidget(self.reg_extra, 1)
        split.addLayout(right, 1)
        lay.addLayout(split, 1)
        b.clicked.connect(self._reg_refresh)
        b2.clicked.connect(self._info_refresh)
        b3.clicked.connect(self._clear_crash)
        return w

    def _tab_target(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        g = QGroupBox("目标控制")
        gl = QGridLayout(g)
        self.t_state = QLabel("目标状态: 未知")
        self.t_state.setObjectName("hint")
        gl.addWidget(self.t_state, 0, 0, 1, 3)
        b_halt = QPushButton("暂停 (Halt)")
        b_resume = QPushButton("恢复 (Resume)")
        b_reset = QPushButton("复位运行 (Reset)")
        self.t_hold = QCheckBox("持续暂停（自动喂 IWDG，防看门狗复位）")
        gl.addWidget(b_halt, 1, 0)
        gl.addWidget(b_resume, 1, 1)
        gl.addWidget(b_reset, 1, 2)
        gl.addWidget(self.t_hold, 2, 0, 1, 3)
        lay.addWidget(g)
        b_halt.clicked.connect(self._target_halt)
        b_resume.clicked.connect(self._target_resume)
        b_reset.clicked.connect(self._target_reset)
        self.t_hold.toggled.connect(self._hold_toggled)
        lay.addStretch(1)
        return w

    def _tab_tools(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        g = QGroupBox("擦除 / 转储 / 比对")
        gl = QGridLayout(g)
        self.x_addr = QLineEdit("080A0000")
        self.x_len = QLineEdit("40000")
        self.x_file = QLineEdit()
        self.x_file.setPlaceholderText("输出/比对文件")
        b_f = QPushButton("浏览...")
        b_f.clicked.connect(lambda: self._pick_file(self.x_file))
        b_er = QPushButton("擦除")
        b_du = QPushButton("转储到文件")
        b_vf = QPushButton("比对文件")
        gl.addWidget(QLabel("地址"), 0, 0)
        gl.addWidget(self.x_addr, 0, 1)
        gl.addWidget(QLabel("长度(HEX)"), 0, 2)
        gl.addWidget(self.x_len, 0, 3)
        gl.addWidget(QLabel("文件"), 1, 0)
        gl.addWidget(self.x_file, 1, 1, 1, 2)
        gl.addWidget(b_f, 1, 3)
        gl.addWidget(b_er, 2, 0)
        gl.addWidget(b_du, 2, 1)
        gl.addWidget(b_vf, 2, 2)
        lay.addWidget(g)
        b_er.clicked.connect(self._tool_erase)
        b_du.clicked.connect(self._tool_dump)
        b_vf.clicked.connect(self._tool_verify)
        lay.addStretch(1)
        return w

    # ---------------- 连接 ----------------

    def _set_ops_enabled(self, en):
        for wdg in (self.btn_flash, self.btn_flash_both, self.btn_cancel,
                    self.btn_ota_all,
                    self.btn_ota_stage, self.btn_ota_trig):
            wdg.setEnabled(en)

    def _connect(self):
        def work():
            # 最多 2 次尝试：首次失败自动 USB 重枚举（免拔插）后重试
            for attempt in (1, 2):
                try:
                    self.log.write("I", "启动 OpenOCD 会话（第 %d 次）..." % attempt)
                    self.session = dc.DapSession(
                        clock_khz=int(self.f_clock.currentText().split()[0]),
                        log=lambda m: self.log.write("D", m))
                    self.session.start()
                    info = self.session.info
                    self.log.write("S", "连接成功: %s  core=%s  flash=%sKB" % (
                        info.get("device_id", "?"), info.get("core", "?"),
                        info.get("flash_kb", "?")))
                    return
                except dc.DapError as e:
                    self.log.write("E", str(e))
                    self.session = None
                    if attempt == 1 and self._recover_usb():
                        self.log.write("I", "已重枚举探针，自动重试...")
                        continue
                    self._connect_error = str(e)
                    return
                except Exception as e:
                    self.log.write("E", "异常: %s" % e)
                    self.session = None
                    self._connect_error = str(e)
                    return

        self._run_thread(work, done=self._on_connect_done)

    def _recover_usb(self):
        """尽力而为的 DAP USB 重枚举（需管理员）；失败不阻塞，交人工重插。"""
        ps = (
            "$d = Get-PnpDevice | Where-Object { $_.FriendlyName -match "
            "'CMSIS-DAP|DAP' -or $_.InstanceId -match 'VID_0D28' }; "
            "if ($d) { $d | Disable-PnpDevice -Confirm:$false -ErrorAction "
            "SilentlyContinue; Start-Sleep -Seconds 2; "
            "$d | Enable-PnpDevice -Confirm:$false -ErrorAction SilentlyContinue }"
        )
        try:
            r = subprocess.run(
                ["powershell", "-NoProfile", "-Command", ps],
                capture_output=True, timeout=12)
            self.log.write("D", "USB 重枚举: rc=%d" % r.returncode)
            time.sleep(1)
            return r.returncode == 0
        except Exception as e:
            self.log.write("D", "USB 重枚举不可用: %s" % e)
            return False

    def _on_connect_done(self):
        if getattr(self, "_connect_error", None):
            QMessageBox.warning(self, "连接失败", self._connect_error)
            self._connect_error = None
            return
        if self.session:
            info = self.session.info
            self.status_pill.setText("● 已连接")
            self.status_pill.setProperty("cls", "ok")
            self._restyle_pill()
            self.info_label.setText(
                "芯片: %s · 内核: %s · Flash: %sKB · DPIDR: %s" % (
                    info.get("device_id", "--"), info.get("core", "--"),
                    info.get("flash_kb", "--"), info.get("dpidr", "--")))
            self.btn_connect.setEnabled(False)
            self.btn_disconnect.setEnabled(True)
            self.btn_refresh.setEnabled(True)
            self._set_ops_enabled(True)
            self.t_state.setText("目标状态: 运行中")

    def _disconnect(self):
        self.cancel_evt.set()
        if self.session:
            try:
                self.session.close()
            except Exception:
                pass
        self.session = None
        self.status_pill.setText("未连接")
        self.status_pill.setProperty("cls", "off")
        self._restyle_pill()
        self.info_label.setText("芯片: --")
        self.btn_connect.setEnabled(True)
        self.btn_disconnect.setEnabled(False)
        self.btn_refresh.setEnabled(False)
        self._set_ops_enabled(False)
        self.log.write("I", "已断开")

    def _refresh_info(self):
        if not self.session:
            return
        self._on_connect_done()

    def _restyle_pill(self):
        self.status_pill.style().unpolish(self.status_pill)
        self.status_pill.style().polish(self.status_pill)

    # ---------------- 操作 ----------------

    def _on_bridge_task(self, kind, payload):
        if kind == "done" and payload:
            payload()
        elif kind == "progress":
            frac, msg = payload
            self.progress.setValue(int(frac * 100))
            self.flash_status.setText(msg)
            self.log.write("I", msg)
        elif kind == "error":
            self.log.write("E", payload)

    def _require(self):
        if self.session is None:
            raise dc.DapError("请先连接 DAP")

    def _run_thread(self, fn, done=None, progress=None):
        def wrapper():
            try:
                fn()
            except dc.DapError as e:
                self.bridge.task.emit("error", str(e))
            except Exception as e:
                self.bridge.task.emit("error", "异常: %s" % e)
            finally:
                if progress:
                    self.bridge.task.emit("progress", (1.0, "结束"))
                if done:
                    self.bridge.task.emit("done", done)

        self.cancel_evt.clear()
        t = threading.Thread(target=wrapper, daemon=True)
        t.start()
        self.op_thread = t

    def _cancel_op(self):
        self.cancel_evt.set()
        self.flash_status.setText("正在取消...")

    def _pick_file(self, line=None):
        path, _ = QFileDialog.getOpenFileName(
            self, "选择文件", r"D:\GIT-SPACE\D00",
            "固件/包 (*.bin);;所有文件 (*)")
        if path:
            (line or self.f_file).setText(path)

    def _addr_combo_changed(self):
        self.f_addr.setEnabled(self.f_addr_combo.currentData() == 0)
        if self.f_addr_combo.currentData():
            self.f_addr.setText("%08X" % self.f_addr_combo.currentData())

    def _parse_hex(self, text):
        return int(text.strip(), 16)

    def _make_app_flash_image(self, raw_path):
        """把原始 APP.bin 补成 320KB 完整 RUN 分区镜像（魔数@0x4FFF8）。
        BOOT 只认 0x0805FFF8 的魔数；直接刷原始 bin 会导致 APP 无效。
        与 workflow 的 append_app_magic.py 同逻辑；版本取 version.json。"""
        raw = open(raw_path, "rb").read()
        if len(raw) >= 320 * 1024 - 8:
            raise dc.DapError("APP.bin 过大，超过魔数区偏移")
        ver = 202
        try:
            import json as _json
            vp = os.path.join(os.path.dirname(os.path.dirname(
                os.path.dirname(os.path.abspath(__file__)))),
                "config", "version.json")
            with open(vp, encoding="utf-8") as f:
                ver = int(_json.load(f).get("ota_version", 202))
        except Exception:
            pass
        image = bytearray(b"\xFF" * (320 * 1024))
        image[:len(raw)] = raw
        struct = __import__("struct")
        struct.pack_into("<I", image, 320 * 1024 - 8, 0x4F54412E)
        struct.pack_into("<I", image, 320 * 1024 - 4, ver)
        tmp = tempfile.NamedTemporaryFile(
            suffix=".bin", prefix="app_flash_", delete=False)
        tmp.write(bytes(image))
        tmp.close()
        self.log.write("I", "已生成完整 RUN 镜像（魔数+版本 v%d，%d 字节）" % (
            ver, len(image)))
        return tmp.name

    def _run_flash(self):
        path = self.f_file.text().strip()
        if not path or not os.path.exists(path):
            QMessageBox.warning(self, "提示", "请先选择固件文件")
            return
        try:
            addr = self._parse_hex(self.f_addr.text())
        except ValueError:
            QMessageBox.warning(self, "提示", "目标地址格式错误")
            return
        chunk_kb = int(self.f_chunk.currentText().split()[0])
        verify = self.f_verify.isChecked()
        self.btn_flash.setEnabled(False)
        self.flash_status.setText("烧录中...")
        flash_path = path
        if addr == 0x08010000 and os.path.getsize(path) < 320 * 1024 - 8:
            try:
                flash_path = self._make_app_flash_image(path)
            except Exception as e:
                QMessageBox.warning(self, "提示", "生成 RUN 镜像失败: %s" % e)
                self.btn_flash.setEnabled(True)
                return

        def progress(frac, msg):
            self.bridge.task.emit("progress", (frac, msg))

        def work():
            self._require()
            self.log.write("S", "开始烧录 %s -> 0x%08X" % (path, addr))
            self.session.flash_file(flash_path, addr, chunk_kb, verify,
                                    progress=progress,
                                    cancel=self.cancel_evt)

        def done():
            self.btn_flash.setEnabled(True)
            self.flash_status.setText("完成")
            self.log.write("S", "烧录完成")

        self._run_thread(work, done=done, progress=progress)

    def _run_flash_both(self):
        """BOOT + APP 一键顺序烧录（各含校验）。"""
        boot = r"D:\GIT-SPACE\D00\BOOT\BOOT\MDK-ARM\BOOT.bin"
        app = r"D:\GIT-SPACE\D00\APP\APP\MDK-ARM\Output\APP.bin"
        if not (os.path.exists(boot) and os.path.exists(app)):
            QMessageBox.warning(self, "提示", "BOOT.bin 或 APP.bin 不存在，请先构建")
            return

        def progress(frac, msg):
            self.bridge.task.emit("progress", (frac, msg))

        def work():
            self._require()
            self.log.write("S", "一键烧录 BOOT -> 0x08000000")
            self.session.flash_file(boot, 0x08000000,
                                    progress=progress, cancel=self.cancel_evt)
            app_img = self._make_app_flash_image(app)
            self.log.write("S", "一键烧录 APP(完整镜像) -> 0x08010000")
            self.session.flash_file(app_img, 0x08010000,
                                    progress=progress, cancel=self.cancel_evt)
            self.log.write("S", "BOOT+APP 一键烧录完成")

        self._run_thread(work)

    def _run_ota(self, mode):
        path = self.o_file.text().strip()
        try:
            addr = self._parse_hex(self.o_addr.text())
        except ValueError:
            QMessageBox.warning(self, "提示", "下载地址格式错误")
            return
        if mode in ("all", "stage") and (not path or not os.path.exists(path)):
            QMessageBox.warning(self, "提示", "请先选择 OTA 加密包")
            return

        def work():
            self._require()
            if mode in ("all", "stage"):
                self.log.write("S", "预置 OTA 包 %s -> 0x%08X" % (path, addr))
                self.session.flash_file(path, addr, progress=lambda f, m: None,
                                        cancel=self.cancel_evt)
            if mode in ("all", "trigger"):
                self.log.write("S", "写 BKP 升级标志 + 复位触发 BOOT 应用...")
                self.session.set_upgrade_flag()
                self.session.cmd("reset run")
                self.log.write("S", "已触发：BOOT 将应用新固件（约 30s 后完成）")

        self._run_thread(work)

    def _mem_read(self):
        def work():
            self._require()
            addr = self._parse_hex(self.m_addr.text())
            n = self.m_len.value()
            vals = self.session.read_words(addr, n)
            lines = []
            for i in range(0, len(vals), 4):
                chunk = vals[i:i + 4]
                lines.append("%08X: %s" % (
                    addr + i * 4,
                    " ".join("%08X" % v for v in chunk)))
            self.m_dump.setPlainText("\n".join(lines))
            self.log.write("S", "内存读取 0x%08X x%d" % (addr, n))

        self._run_thread(work)

    def _mem_write(self):
        def work():
            self._require()
            addr = self._parse_hex(self.m_addr2.text())
            val = self._parse_hex(self.m_val.text())
            self.session.write_u32(addr, val)
            self.log.write("S", "已写入 0x%08X <- 0x%08X" % (addr, val))

        self._run_thread(work)

    def _reg_refresh(self):
        def work():
            self._require()
            regs = self.session.read_regs()
            rows = []
            for i in range(13):
                rows.append(("r%d" % i, regs.get("r%d" % i, 0)))
            for name in ("sp", "lr", "pc", "xpsr", "primask", "control"):
                rows.append((name, regs.get(name, 0)))
            bkp = self.session.read_rtc_bkp()
            self.reg_table.setRowCount(len(rows))
            for r, (name, val) in enumerate(rows):
                self.reg_table.setItem(r, 0, QTableWidgetItem(name))
                self.reg_table.setItem(r, 1,
                                       QTableWidgetItem("0x%08X" % val))
            extra = "BKP0R-3R: " + "  ".join(
                "0x%08X" % v for v in bkp) if bkp else "BKP: --"
            self.reg_extra.setPlainText(extra)
            self.log.write("S", "寄存器刷新完成 (pc=0x%08X)" %
                           regs.get("pc", 0))

        self._run_thread(work)

    def _info_refresh(self):
        def work():
            self._require()
            lines = []
            uid = self.session.device_uid()
            if uid:
                b = b"".join(bytes(((x >> 0) & 0xFF, (x >> 8) & 0xFF,
                                    (x >> 16) & 0xFF, (x >> 24) & 0xFF))
                             for x in uid)
                lines.append("UID : %s" % b.hex())
            fi = self.session.firmware_info()
            if fi:
                lines.append("固件: 魔数=0x%08X 版本=%s 最后应用构建=%s" % (
                    fi.get("magic", 0), fi.get("version", "?"),
                    fi.get("last_build", "?")))
            cr = self.session.crash_record()
            if cr:
                lines.append("崩溃: #%d %s task=%s pc=0x%08X lr=0x%08X "
                             "uptime=%dms" % (
                                 cr["seq"], cr["src_name"], cr["task"],
                                 cr["pc"], cr["lr"], cr["tick"]))
                lines.append("  CFSR: %s" % cr["cfsr_text"])
                lines.append("  HFSR: %s" % cr["hfsr_text"])
            else:
                lines.append("崩溃: 无有效记录")
            self.reg_extra.setPlainText("\n".join(lines))
            self.log.write("S", "设备/固件/崩溃信息刷新完成")

        self._run_thread(work)

    def _clear_crash(self):
        """清除 RTC 备份寄存器里的崩溃记录（BKP1R-12R 写 0）。"""
        def work():
            self._require()
            self.session.cmd("reset halt")
            self.session.cmd("set _apb [mrw 0x40023840]")
            self.session.cmd("mww 0x40023840 [expr {$_apb | 0x10000000}]")
            self.session.cmd("set _cr [mrw 0x40007000]")
            self.session.cmd("mww 0x40007000 [expr {$_cr | 0x100}]")
            for i in range(1, 13):
                self.session.cmd("mww 0x%X 0x0" % (dc.RTC_BKP0R + i * 4))
            self.session.cmd("reset run")
            self.log.write("S", "崩溃记录已清除（BKP1R-12R）")

        self._run_thread(work)

    def _tasks_refresh(self):
        def work():
            self._require()
            tasks = self.session.rtos_tasks()
            self.tk_table.setRowCount(len(tasks))
            for r, t in enumerate(sorted(tasks, key=lambda x: (-x["prio"],
                                                              x["name"]))):
                for c, key in enumerate(("name", "prio", "state",
                                         "stack_used", "tcb")):
                    val = t[key]
                    if key == "prio":
                        text = str(val)
                    elif key == "stack_used":
                        text = "%d" % val
                    elif key == "tcb":
                        text = "0x%08X" % val
                    else:
                        text = str(val)
                    self.tk_table.setItem(r, c, QTableWidgetItem(text))
            self.tk_info.setText("共 %d 个任务（就绪/延时/当前）" % len(tasks))
            self.log.write("S", "RTOS 任务刷新完成")

        self._run_thread(work)

    def _bp_set(self):
        def work():
            self._require()
            addr = self._parse_hex(self.d_bp_addr.text())
            self.session.bp_set(addr)
            self.log.write("S", "断点已设 0x%08X" % addr)
            self._bp_list()

        self._run_thread(work)

    def _bp_clear(self):
        def work():
            self._require()
            addr = self._parse_hex(self.d_bp_addr.text())
            self.session.bp_clear(addr)
            self.log.write("S", "断点已清 0x%08X" % addr)
            self._bp_list()

        self._run_thread(work)

    def _bp_list(self):
        bps = self.session.bp_list()
        self.d_bp_info.setText("断点: " + (" ".join("0x%08X" % b for b in bps)
                                           if bps else "无"))
        self.log.write("I", "当前断点: %s" % self.d_bp_info.text())

    def _debug_step(self):
        def work():
            self._require()
            self.session.step()
            self.log.write("S", "单步执行")

        self._run_thread(work)

    def _debug_disasm(self):
        def work():
            self._require()
            addr = self._parse_hex(self.d_dis_addr.text())
            out = self.session.disasm(addr, self.d_dis_n.value())
            # 提取反汇编行
            lines = []
            for ln in out.splitlines():
                ln = ln.strip()
                if re_asm(ln):
                    lines.append(ln)
            self.d_dis.setPlainText("\n".join(lines) if lines else out)
            self.log.write("S", "反汇编 0x%08X x%d" % (
                addr, self.d_dis_n.value()))

        self._run_thread(work)

    def _debug_fault(self):
        def work():
            self._require()
            f = self.session.fault_regs()
            txt = ("CFSR = 0x%08X  %s\n"
                   "HFSR = 0x%08X  %s\n"
                   "DFSR = 0x%08X   BFAR=0x%08X  MMFAR=0x%08X\n"
                   "复位原因: %s" % (
                       f["cfsr"], f["cfsr_text"], f["hfsr"], f["hfsr_text"],
                       f["dfsr"], f["bfar"], f["mmfar"],
                       f["reset_reason"]))
            self.d_fault.setPlainText(txt)
            self.log.write("S", "故障/复位诊断完成")

        self._run_thread(work)

    def _target_halt(self):
        def work():
            self._require()
            self.session.cmd("halt")
            self.session.cmd("mww 0x%X 0xAAAA" % dc.IWDG_KR)
            self.t_state.setText("目标状态: 已暂停")
            self.log.write("S", "目标已暂停")

        self._run_thread(work)

    def _target_resume(self):
        def work():
            self._require()
            self.session.cmd("resume")
            self.t_state.setText("目标状态: 运行中")
            self.log.write("S", "目标已恢复运行")

        self._run_thread(work)

    def _target_reset(self):
        def work():
            self._require()
            self.session.cmd("reset run")
            self.t_state.setText("目标状态: 运行中（已复位）")
            self.log.write("S", "目标已复位运行")

        self._run_thread(work)

    def _hold_toggled(self, on):
        if on:
            self._require()
            self.session.cmd("halt")
            self.t_state.setText("目标状态: 持续暂停")
            self.hold_timer.start(1500)
        else:
            self.hold_timer.stop()
            if self.session:
                self.session.cmd("resume")
                self.t_state.setText("目标状态: 运行中")

    def _hold_feed(self):
        if self.session:
            try:
                self.session.cmd("mww 0x%X 0xAAAA" % dc.IWDG_KR)
            except Exception:
                pass

    def _tool_erase(self):
        def work():
            self._require()
            addr = self._parse_hex(self.x_addr.text())
            ln = self._parse_hex(self.x_len.text())
            start, slen = self.session.erase(addr, ln)
            self.log.write("S", "已擦除 0x%08X len 0x%X（实际扇区 %d）" %
                           (start, slen, slen // 0x4000))

        self._run_thread(work)

    def _tool_dump(self):
        path = self.x_file.text().strip()
        if not path:
            QMessageBox.warning(self, "提示", "请选择输出文件")
            return

        def work():
            self._require()
            addr = self._parse_hex(self.x_addr.text())
            ln = self._parse_hex(self.x_len.text())
            self.session.dump(addr, ln, path)
            self.log.write("S", "已转储 0x%X 字节到 %s" % (ln, path))

        self._run_thread(work)

    def _tool_verify(self):
        path = self.x_file.text().strip()
        if not path or not os.path.exists(path):
            QMessageBox.warning(self, "提示", "请选择比对文件")
            return

        def work():
            self._require()
            addr = self._parse_hex(self.x_addr.text())
            ok = self.session.verify_file(path, addr)
            self.log.write("S" if ok else "E",
                           "比对%s: %s @ 0x%08X" %
                           ("通过" if ok else "不一致", path, addr))

        self._run_thread(work)

    def closeEvent(self, ev):
        self.cancel_evt.set()
        if self.session:
            try:
                self.session.close()
            except Exception:
                pass
        super().closeEvent(ev)


def main():
    QApplication.setAttribute(Qt.AA_EnableHighDpiScaling, True)
    QApplication.setAttribute(Qt.AA_UseHighDpiPixmaps, True)
    app = QApplication(sys.argv)
    qss = os.path.join(os.path.dirname(os.path.abspath(__file__)), "styles.qss")
    if os.path.exists(qss):
        with open(qss, encoding="utf-8") as f:
            app.setStyleSheet(f.read())
    w = MainWindow()
    w.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
