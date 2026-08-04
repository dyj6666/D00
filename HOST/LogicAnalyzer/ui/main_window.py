"""逻辑分析仪主窗口"""
from __future__ import annotations

import threading
import time
from pathlib import Path

import serial
import serial.tools.list_ports
import numpy as np
from pyqtgraph.Qt import QtCore, QtWidgets

from la.capture import CaptureError, CaptureSession
from la import decoders as dec
from model.packets import DecodeResult
from model.trace import TraceData
from ui.detail_panel import DetailPanel
from ui.packet_panel import PacketPanel
from ui.waveform import WaveformWidget


class CaptureThread(QtCore.QThread):
    finished_ok = QtCore.Signal(object)
    failed = QtCore.Signal(str)
    progressed = QtCore.Signal(int, int)

    def __init__(self, session, rate, duration, buffer_src, count, trig_args,
                 parent=None):
        super().__init__(parent)
        self._s = (session, rate, duration, buffer_src, count, trig_args)

    def run(self):
        try:
            session, rate, duration, buf, count, trig = self._s
            trace = session.capture(rate, duration, buf, count,
                                    progress=self.progressed.emit,
                                    trig_args=trig)
            self.finished_ok.emit(trace)
        except Exception as e:  # noqa: BLE001
            self.failed.emit(str(e))


class DecodeThread(QtCore.QThread):
    finished_ok = QtCore.Signal(object)

    def __init__(self, trace, protocol, cfg, parent=None):
        super().__init__(parent)
        self._t = (trace, protocol, cfg)

    def run(self):
        trace, protocol, cfg = self._t
        if protocol == "UART":
            packets = dec.decode_uart(trace.samples, trace.rate, cfg)
        elif protocol == "I2C":
            packets = dec.decode_i2c(trace.samples, trace.rate, cfg)
        elif protocol == "SPI":
            packets = dec.decode_spi(trace.samples, trace.rate, cfg)
        else:
            packets = []
        self.finished_ok.emit(DecodeResult(protocol, packets))


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("LogicAnalyzer Pro — STM32F407 8CH")
        self.resize(1500, 900)
        self.session: CaptureSession | None = None
        self.trace: TraceData | None = None
        self._build_ui()
        self._apply_style()

    # ---------------- UI ----------------
    def _build_ui(self):
        self.wave = WaveformWidget()
        self.wave.sampleSelected.connect(self._on_sample)
        self.wave.regionSelected.connect(self._on_region)
        self.setCentralWidget(self.wave)

        self.packet_panel = PacketPanel()
        self.packet_panel.packetClicked.connect(self.wave.highlight)
        self.detail_panel = DetailPanel()

        tabs = QtWidgets.QTabWidget()
        tabs.addTab(self.packet_panel, "数据包")
        tabs.addTab(self.detail_panel, "位视图")
        self.addDockWidget(QtCore.Qt.BottomDockWidgetArea, self._dock(tabs, "结果"))

        self.addDockWidget(QtCore.Qt.RightDockWidgetArea,
                           self._dock(self._build_control_panel(), "控制与解码"))

        self.statusBar().showMessage("就绪")

    def _dock(self, widget, title):
        dock = QtWidgets.QDockWidget(title, self)
        dock.setWidget(widget)
        dock.setFeatures(QtWidgets.QDockWidget.DockWidgetMovable |
                         QtWidgets.QDockWidget.DockWidgetFloatable)
        return dock

    def _build_control_panel(self) -> QtWidgets.QWidget:
        panel = QtWidgets.QWidget()
        form = QtWidgets.QFormLayout(panel)

        self.ctrl_port = QtWidgets.QComboBox()
        self.data_port = QtWidgets.QComboBox()
        refresh = QtWidgets.QPushButton("刷新")
        refresh.clicked.connect(self._refresh_ports)
        self._refresh_ports()
        row = QtWidgets.QWidget()
        hl = QtWidgets.QHBoxLayout(row)
        hl.setContentsMargins(0, 0, 0, 0)
        hl.addWidget(self.ctrl_port)
        hl.addWidget(refresh)
        form.addRow("控制口", row)
        form.addRow("数据口", self.data_port)

        self.rate_combo = QtWidgets.QComboBox()
        self.rate_combo.addItems(
            ["100000", "500000", "1000000", "2000000", "5000000",
             "10000000"])
        self.rate_combo.setEditable(True)
        form.addRow("采样率 Hz", self.rate_combo)

        self.buffer_combo = QtWidgets.QComboBox()
        self.buffer_combo.addItems(["sram", "iram"])
        form.addRow("缓冲", self.buffer_combo)

        self.duration_spin = QtWidgets.QDoubleSpinBox()
        self.duration_spin.setRange(0.05, 60.0)
        self.duration_spin.setValue(1.0)
        self.duration_spin.setSuffix(" s")
        form.addRow("采集时长", self.duration_spin)

        self.count_spin = QtWidgets.QSpinBox()
        self.count_spin.setRange(0, 32768)
        self.count_spin.setValue(0)
        self.count_spin.setSpecialValueText("全部")
        form.addRow("下载点数(0=全部)", self.count_spin)

        self.trig_edit = QtWidgets.QLineEdit()
        self.trig_edit.setPlaceholderText("如: 1 4 2048 2 0 (type ch post cond_ch cond_lv)")
        form.addRow("触发(la_trig)", self.trig_edit)

        self.progress = QtWidgets.QProgressBar()
        self.progress.setRange(0, 100)
        form.addRow("下载进度", self.progress)

        btn_row = QtWidgets.QWidget()
        bl = QtWidgets.QHBoxLayout(btn_row)
        bl.setContentsMargins(0, 0, 0, 0)
        self.start_btn = QtWidgets.QPushButton("开始采集")
        self.start_btn.setObjectName("primary")
        self.start_btn.clicked.connect(self._start_capture)
        self.stop_btn = QtWidgets.QPushButton("停止")
        self.stop_btn.setObjectName("danger")
        self.stop_btn.setEnabled(False)
        self.stop_btn.clicked.connect(self._stop_capture)
        bl.addWidget(self.start_btn)
        bl.addWidget(self.stop_btn)
        form.addRow(btn_row)

        sep = QtWidgets.QFrame()
        sep.setFrameShape(QtWidgets.QFrame.HLine)
        form.addRow(sep)

        self.proto_combo = QtWidgets.QComboBox()
        self.proto_combo.addItems(["UART", "I2C", "SPI"])
        self.proto_combo.currentTextChanged.connect(self._update_decoder_form)
        form.addRow("解码协议", self.proto_combo)

        self.decoder_form = QtWidgets.QFormLayout()
        wrap = QtWidgets.QWidget()
        wrap.setLayout(self.decoder_form)
        form.addRow(wrap)

        self.decode_btn = QtWidgets.QPushButton("解码")
        self.decode_btn.setObjectName("primary")
        self.decode_btn.clicked.connect(self._decode)
        form.addRow(self.decode_btn)

        export_row = QtWidgets.QWidget()
        el = QtWidgets.QHBoxLayout(export_row)
        el.setContentsMargins(0, 0, 0, 0)
        self.export_raw_btn = QtWidgets.QPushButton("导出原始")
        self.export_raw_btn.clicked.connect(self._export_raw)
        self.export_pkt_btn = QtWidgets.QPushButton("导出数据包")
        self.export_pkt_btn.clicked.connect(self._export_packets)
        el.addWidget(self.export_raw_btn)
        el.addWidget(self.export_pkt_btn)
        form.addRow(export_row)

        self._decoder_spins: dict = {}
        self._update_decoder_form("UART")
        return panel

    def _refresh_ports(self):
        ports = serial.tools.list_ports.comports()
        names = [p.device for p in ports]
        self.ctrl_port.clear()
        self.data_port.clear()
        self.ctrl_port.addItems(names)
        self.data_port.addItems(names)
        for combo, pref in ((self.ctrl_port, "COM9"),
                            (self.data_port, "COM13")):
            idx = combo.findText(pref)
            if idx >= 0:
                combo.setCurrentIndex(idx)

    def _update_decoder_form(self, protocol: str):
        # 清空旧控件
        while self.decoder_form.rowCount():
            self.decoder_form.removeRow(0)
        self._decoder_spins = {}

        def spin(name, lo, hi, val, suffix=""):
            s = QtWidgets.QSpinBox()
            s.setRange(lo, hi)
            s.setValue(val)
            if suffix:
                s.setSuffix(suffix)
            self._decoder_spins[name] = s
            self.decoder_form.addRow(name, s)

        if protocol == "UART":
            spin("波特率", 1200, 4000000, 115200)
            spin("TX 通道", 0, 7, 0)
            spin("RX 通道", 0, 7, 1)
        elif protocol == "I2C":
            spin("SCL 通道", 0, 7, 0)
            spin("SDA 通道", 0, 7, 1)
        elif protocol == "SPI":
            spin("CLK 通道", 0, 7, 0)
            spin("MOSI 通道", 0, 7, 1)
            spin("MISO 通道", 0, 7, 2)
            spin("CS 通道", 0, 7, 3)
            spin("CPOL", 0, 1, 0)
            spin("CPHA", 0, 1, 0)

    # ---------------- 采集 ----------------
    def _start_capture(self):
        try:
            rate = int(self.rate_combo.currentText())
        except ValueError:
            self.statusBar().showMessage("采样率无效")
            return
        if self.session is None:
            try:
                self.session = CaptureSession(
                    self.ctrl_port.currentText(), self.data_port.currentText())
            except Exception as e:  # noqa: BLE001
                self.statusBar().showMessage(f"串口打开失败: {e}")
                return
        trig = self.trig_edit.text().strip() or None
        count = self.count_spin.value()
        self.start_btn.setEnabled(False)
        self.stop_btn.setEnabled(True)
        self.progress.setValue(0)
        self._thread = CaptureThread(
            self.session, rate, self.duration_spin.value(),
            self.buffer_combo.currentText(), count, trig)
        self._thread.progressed.connect(self._on_progress)
        self._thread.finished_ok.connect(self._on_capture_done)
        self._thread.failed.connect(self._on_capture_failed)
        self._thread.start()

    def _stop_capture(self):
        if self.session:
            try:
                self.session.ctrl.la_stop_dma()
            except Exception:  # noqa: BLE001
                pass
        self.statusBar().showMessage("已请求停止")

    def _on_progress(self, done, total):
        self.progress.setValue(int(done / total * 100) if total else 0)

    def _on_capture_done(self, trace: TraceData):
        self.trace = trace
        self.wave.set_trace(trace)
        self.detail_panel.set_trace(trace)
        self.packet_panel.clear()
        self.start_btn.setEnabled(True)
        self.stop_btn.setEnabled(False)
        self.statusBar().showMessage(
            f"采集完成: {trace.count} 样本, {trace.duration_us / 1e6:.3f} s "
            f"@{trace.rate} Hz, 8 通道  ——  拖动蓝色测量区查看频率/占空比")
        self.wave.fit_all()

    def _on_capture_failed(self, msg: str):
        self.start_btn.setEnabled(True)
        self.stop_btn.setEnabled(False)
        self.statusBar().showMessage(f"采集失败: {msg}")

    # ---------------- 解码 ----------------
    def _decode(self):
        if self.trace is None:
            self.statusBar().showMessage("先采集数据")
            return
        proto = self.proto_combo.currentText()
        s = self._decoder_spins
        if proto == "UART":
            cfg = dec.UartConfig(tx_ch=s["TX 通道"].value(),
                                 rx_ch=s["RX 通道"].value(),
                                 baud=s["波特率"].value())
        elif proto == "I2C":
            cfg = dec.I2cConfig(scl_ch=s["SCL 通道"].value(),
                                sda_ch=s["SDA 通道"].value())
        else:
            cfg = dec.SpiConfig(clk_ch=s["CLK 通道"].value(),
                                mosi_ch=s["MOSI 通道"].value(),
                                miso_ch=s["MISO 通道"].value(),
                                cs_ch=s["CS 通道"].value(),
                                cpol=s["CPOL"].value(),
                                cpha=s["CPHA"].value())
        self.statusBar().showMessage(f"正在解码 {proto} ...")
        self._dthread = DecodeThread(self.trace, proto, cfg)
        self._dthread.finished_ok.connect(self._on_decode_done)
        self._dthread.start()

    def _on_decode_done(self, result: DecodeResult):
        self.packet_panel.set_packets(result.packets, self.trace.rate)
        self.statusBar().showMessage(
            f"{result.protocol} 解码完成: {len(result.packets)} 个数据包")

    # ---------------- 视图联动 ----------------
    def _on_sample(self, idx: int):
        self.detail_panel.show_sample(idx)

    def _on_region(self, a: int, b: int):
        self.detail_panel.show_range(a, b)
        ms = []
        for ch in range(min(self.trace.nchannels, 8)):
            m = self.wave.measure(ch, a, b)
            if m and m["rising"] > 0:
                if m["freq"] > 0:
                    ms.append(f"CH{ch}: {m['freq']:.0f} Hz  占空比 {m['duty']:.1%}  "
                              f"({m['us']:.0f} µs)")
                else:
                    ms.append(f"CH{ch}: 边缘不足，请拖宽测量区  "
                              f"(区间 {m['us']:.0f} µs)")
        self.detail_panel.show_measure(ms)

    # ---------------- 导出 ----------------
    def _export_raw(self):
        if self.trace is None:
            return
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "导出原始采样", "la_trace.csv", "CSV (*.csv)")
        if not path:
            return
        rate = self.trace.rate
        with open(path, "w", newline="") as f:
            f.write("index,time_us," + ",".join(f"ch{i}" for i in range(8)) + "\n")
            t_us = np.arange(self.trace.count) / rate * 1e6
            for i in range(0, self.trace.count, 1):
                s = self.trace.samples[i]
                f.write(f"{i},{t_us[i]:.3f},"
                        + ",".join(str((s >> ch) & 1) for ch in range(8)) + "\n")
        self.statusBar().showMessage(f"已导出: {path}")

    def _export_packets(self):
        rows = self.packet_panel.table.rowCount()
        if rows == 0:
            self.statusBar().showMessage("无数据包可导出")
            return
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "导出数据包", "la_packets.csv", "CSV (*.csv)")
        if not path:
            return
        with open(path, "w", newline="") as f:
            f.write("kind,start,end,data,info\n")
            for r in range(rows):
                f.write(",".join(
                    self.packet_panel.table.item(r, c).text().replace(",", " ")
                    for c in range(6)) + "\n")
        self.statusBar().showMessage(f"已导出: {path}")

    def _apply_style(self):
        qss = Path(__file__).parent / "style.qss"
        if qss.exists():
            self.setStyleSheet(qss.read_text(encoding="utf-8"))

    def closeEvent(self, event):
        if self.session:
            self.session.close()
        event.accept()
