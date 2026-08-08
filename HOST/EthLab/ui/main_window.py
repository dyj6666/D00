# -*- coding: utf-8 -*-
"""EthLab 主窗口：控制台 + 实时抓包 + 帧结构 + 统计。"""

import json
import re
from pathlib import Path

from PyQt5.QtCore import Qt
from PyQt5.QtWidgets import (QDialog, QDialogButtonBox, QFileDialog, QLabel,
                             QMainWindow, QMessageBox, QPlainTextEdit,
                             QPushButton, QSplitter, QStatusBar, QTabWidget,
                             QToolBar, QVBoxLayout, QWidget)

from ui.capture_panel import (CaptureListener, CapturePanel,
                              parse_capture_datagram)
from ui.console_panel import ConsolePanel
from ui.detail_panel import DetailPanel
from ui.echo_dialog import EchoDialog
from ui.stats_panel import StatsPanel
from ui.style import QSS


class OfflineParseDialog(QDialog):
    """离线解析：粘贴 TX/RX 行或纯 hex。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("离线解析帧")
        self.setMinimumSize(560, 380)
        self._frames = []
        lay = QVBoxLayout(self)
        lay.addWidget(QLabel(
            "粘贴帧（每行: TX 74B: AA BB ... 或 RX 74B: ...，也可粘贴纯 hex）："))
        self.editor = QPlainTextEdit()
        self.editor.setPlaceholderText(
            "TX 74B: 00 E0 4C 25 29 88 02 00 11 22 33 44 08 00 ...\n"
            "或直接粘贴十六进制（可含换行/空格）")
        lay.addWidget(self.editor, 1)
        btns = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        btns.accepted.connect(self._parse)
        btns.rejected.connect(self.reject)
        lay.addWidget(btns)

    def _parse(self):
        frames = []
        direction = "TX"
        for raw_line in self.editor.toPlainText().splitlines():
            ln = raw_line.strip()
            if not ln:
                continue
            m = re.match(r"^(TX|RX)\s+(\d+)B:\s*([0-9A-Fa-f ]+)$", ln)
            if m:
                direction = m.group(1)
                hexstr = m.group(3)
            else:
                hexstr = ln
            try:
                raw = bytes.fromhex(re.sub(r"[^0-9A-Fa-f]", "", hexstr))
            except ValueError:
                continue
            if raw:
                frames.append((direction, raw))
        if not frames:
            QMessageBox.information(self, "EthLab", "未解析到有效帧")
            return
        self._frames = frames
        self.accept()


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("EthLab - D00 以太网分析控制台")
        self.resize(1360, 860)

        self.console = ConsolePanel()
        self.capture = CapturePanel()
        self.detail = DetailPanel()
        self.stats = StatsPanel()

        right = QTabWidget()
        right.addTab(self.capture, "实时抓包")
        right.addTab(self.detail, "帧结构")
        right.addTab(self.stats, "统计")

        self.capture.frame_selected.connect(self._on_frame_selected)
        self.capture.frame_added.connect(self.stats.on_frame)
        self.console.telemetry.connect(self._on_telemetry)

        split = QSplitter(Qt.Horizontal)
        split.addWidget(self.console)
        split.addWidget(right)
        split.setSizes([440, 920])
        self.setCentralWidget(split)

        self._listener = CaptureListener(7778, self)
        self._listener.datagram.connect(self._on_datagram)
        self._listener.start()

        self._build_toolbar()
        self._build_statusbar()
        self.setStyleSheet(QSS)

    def _build_toolbar(self):
        tb = QToolBar("工具")
        tb.setMovable(False)
        self.addToolBar(tb)
        for label, fn in (("保存捕获", self._save_capture),
                          ("加载捕获", self._load_capture),
                          ("导出 PCAP", self._export_pcap),
                          ("离线解析", self._offline_parse),
                          ("UDP 回显测试", self._echo_test),
                          ("清空抓包", self.capture.clear)):
            b = QPushButton(label)
            b.clicked.connect(fn)
            tb.addWidget(b)

    def _build_statusbar(self):
        sb = QStatusBar()
        self.setStatusBar(sb)
        self.stat_conn = QLabel("控制台: 未连接")
        self.stat_cap = QLabel("抓帧: 监听 0.0.0.0:7778")
        self.stat_tele = QLabel("UPTIME - | HEAP - | TASKS - | ETH -")
        self.stat_err = QLabel("校验和错误: 0")
        sb.addWidget(self.stat_conn)
        sb.addPermanentWidget(self.stat_cap)
        sb.addPermanentWidget(self.stat_tele)
        sb.addPermanentWidget(self.stat_err)

    def _on_telemetry(self, kv):
        parts = []
        for k in ("UPTIME", "HEAP", "TASKS", "ETH"):
            if k in kv:
                parts.append("%s %s" % (k, kv[k]))
        if parts:
            self.stat_tele.setText(" | ".join(parts))
        if kv.get("ETH", "").startswith("UP"):
            self.stat_conn.setText("控制台: 已连接 (链路 UP)")
        else:
            self.stat_conn.setText("控制台: 已连接")

    def _on_datagram(self, data):
        if data.startswith(b"ERR:"):
            self.stat_cap.setText("抓帧: " + data[4:].decode("utf-8", "replace"))
            return
        parsed = parse_capture_datagram(data)
        if parsed is None:
            return
        direction, raw, orig, trunc = parsed
        self.capture.add_frame(direction, raw, orig, trunc)
        self.stat_cap.setText("抓帧: 监听 0.0.0.0:7778 | 已收 %d 帧" %
                              self.capture.count())

    def _on_frame_selected(self, fr):
        self.detail.show_frame(fr)
        errs = sum(1 for f in fr.fields if "FAIL" in f.desc)
        if errs:
            self.stat_err.setText("校验和错误: 选中帧 %d 处" % errs)

    # ---- 捕获管理 ----

    def _save_capture(self):
        path, _ = QFileDialog.getSaveFileName(self, "保存捕获", "capture.json",
                                              "JSON (*.json)")
        if not path:
            return
        n = self.capture.save_json(path)
        self.statusBar().showMessage("捕获已保存: %s (%d 帧)" % (path, n), 4000)

    def _load_capture(self):
        path, _ = QFileDialog.getOpenFileName(self, "加载捕获", "",
                                              "JSON (*.json)")
        if not path:
            return
        try:
            n = self.capture.load_json(path)
            self.statusBar().showMessage("已加载 %d 帧" % n, 4000)
        except Exception as e:
            QMessageBox.warning(self, "EthLab", "加载失败: %s" % e)

    def _export_pcap(self):
        path, _ = QFileDialog.getSaveFileName(self, "导出 PCAP", "capture.pcap",
                                              "PCAP (*.pcap)")
        if not path:
            return
        try:
            n = self.capture.export_pcap(path)
            self.statusBar().showMessage("已导出 %d 帧 -> %s (可用 Wireshark 打开)"
                                         % (n, path), 4000)
        except Exception as e:
            QMessageBox.warning(self, "EthLab", "导出失败: %s" % e)

    def _offline_parse(self):
        dlg = OfflineParseDialog(self)
        if dlg.exec_() == QDialog.Accepted:
            for direction, raw in dlg._frames:
                self.capture.add_frame(direction, raw)
            self.statusBar().showMessage("离线解析 %d 帧" % len(dlg._frames), 4000)

    def _echo_test(self):
        dlg = EchoDialog(self, default_ip=self.console.ip_edit.text().strip())
        dlg.exec_()

    def closeEvent(self, ev):
        self._listener.stop()
        self._listener.wait(1500)
        self.console.close_conn()
        super().closeEvent(ev)
