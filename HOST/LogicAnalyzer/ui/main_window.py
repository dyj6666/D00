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
                 nchannels, parent=None):
        super().__init__(parent)
        self._s = (session, rate, duration, buffer_src, count, trig_args,
                   nchannels)

    def run(self):
        try:
            session, rate, duration, buf, count, trig, nch = self._s
            trace = session.capture(rate, duration, buf, count,
                                    progress=self.progressed.emit,
                                    trig_args=trig, nchannels=nch)
            self.finished_ok.emit(trace)
        except Exception as e:  # noqa: BLE001
            self.failed.emit(str(e))


class DecodeThread(QtCore.QThread):
    finished_ok = QtCore.Signal(object)
    failed = QtCore.Signal(str)

    def __init__(self, trace, protocol, cfg, parent=None):
        super().__init__(parent)
        self._t = (trace, protocol, cfg)

    def run(self):
        try:
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
        except Exception as e:  # noqa: BLE001
            self.failed.emit(str(e))


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("LogicAnalyzer Pro — STM32F407")
        self.resize(1500, 900)
        self.session: CaptureSession | None = None
        self.trace: TraceData | None = None
        self._last_decode: tuple | None = None
        self._pending_region: tuple[int, int] | None = None
        self._region_timer = QtCore.QTimer(self)
        self._region_timer.setSingleShot(True)
        self._region_timer.setInterval(50)
        self._region_timer.timeout.connect(self._region_flush)
        self._build_ui()
        self._apply_style()

    # ---------------- UI ----------------
    def _build_ui(self):
        self.wave = WaveformWidget()
        self.wave.sampleSelected.connect(self._on_sample)
        self.wave.regionSelected.connect(self._on_region)
        self.wave.doubleClickedAt.connect(self._on_wave_double_click)
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

        self.channels_combo = QtWidgets.QComboBox()
        self.channels_combo.addItems(["4", "8"])
        self.channels_combo.setCurrentIndex(0)
        self.channels_combo.currentIndexChanged.connect(
            lambda _: self._update_decoder_form(self.proto_combo.currentText()))
        form.addRow("通道数", self.channels_combo)

        self.buffer_combo = QtWidgets.QComboBox()
        self.buffer_combo.addItems(["sram", "iram"])
        # 默认 IRAM：本板 PB1~PB3/PB5~PB7 与外部 SRAM 总线相连，
        # SRAM 缓冲模式会采到总线写地址的计数序列（非真实信号）
        self.buffer_combo.setCurrentIndex(1)
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

        self.region_check = QtWidgets.QCheckBox("显示测量区（可拖动）")
        self.region_check.setChecked(True)
        self.region_check.toggled.connect(
            lambda on: self.wave.show_region(on))
        form.addRow(self.region_check)

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

        self.annotate_check = QtWidgets.QCheckBox("波形协议位标注")
        self.annotate_check.setChecked(True)
        self.annotate_check.toggled.connect(self._refresh_annotations)
        form.addRow(self.annotate_check)

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
        nch = int(self.channels_combo.currentText())

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
            spin("TX 通道", 0, nch - 1, 0)
            spin("RX 通道", 0, nch - 1, 1)
        elif protocol == "I2C":
            spin("SCL 通道", 0, nch - 1, 0)
            spin("SDA 通道", 0, nch - 1, 1)
        elif protocol == "SPI":
            spin("CLK 通道", 0, nch - 1, 0)
            spin("MOSI 通道", 0, nch - 1, 1)
            spin("MISO 通道", 0, nch - 1, 2)
            spin("CS 通道", 0, nch - 1, 3)
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
            self.buffer_combo.currentText(), count, trig,
            int(self.channels_combo.currentText()))
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
        self.wave.clear_protocol_annotations()
        self._last_decode = None
        self.start_btn.setEnabled(True)
        self.stop_btn.setEnabled(False)
        self.statusBar().showMessage(
            f"采集完成: {trace.count} 样本, {trace.duration_us / 1e6:.3f} s "
            f"@{trace.rate} Hz, {trace.nchannels} 通道  ——  "
            f"拖动蓝色测量区查看频率/占空比")
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
            baud = s["波特率"].value()
            if self.trace.rate / baud < 4.0:
                self.statusBar().showMessage(
                    f"解码失败: 采样率 {self.trace.rate} Hz 相对波特率 "
                    f"{baud} 过低（每 bit 不足 4 样本），请将采样率提高到 "
                    f"{baud * 4} Hz 以上再解码")
                return
            cfg = dec.UartConfig(tx_ch=s["TX 通道"].value(),
                                 rx_ch=s["RX 通道"].value(),
                                 baud=baud)
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
        self._last_decode = (proto, cfg)
        self._dthread = DecodeThread(self.trace, proto, cfg)
        self._dthread.finished_ok.connect(self._on_decode_done)
        self._dthread.failed.connect(self._on_decode_failed)
        self._dthread.start()

    def _on_decode_failed(self, msg: str):
        self.statusBar().showMessage(f"解码失败: {msg}")

    def _on_decode_done(self, result: DecodeResult):
        self.packet_panel.set_packets(result.packets, self.trace.rate)
        self.statusBar().showMessage(
            f"{result.protocol} 解码完成: {len(result.packets)} 个数据包")
        self._refresh_annotations()

    def _refresh_annotations(self):
        if self.trace is None or self._last_decode is None:
            return
        proto, cfg = self._last_decode
        self.wave.set_channel_names(self._channel_names(proto, cfg))
        self.detail_panel.set_protocol(proto, cfg)
        if not self.annotate_check.isChecked():
            self.wave.clear_protocol_annotations()
            return
        try:
            if proto == "UART":
                annos = dec.annotate_uart(self.trace.samples, self.trace.rate, cfg)
            elif proto == "I2C":
                annos = dec.annotate_i2c(self.trace.samples, self.trace.rate, cfg)
            elif proto == "SPI":
                annos = dec.annotate_spi_bits(self.trace.samples,
                                              self.trace.rate, cfg)
            else:
                annos = []
        except Exception:  # noqa: BLE001
            annos = []
        self.wave.set_protocol_annotations(annos)

    def _channel_names(self, proto: str, cfg) -> list:
        """按解码配置把通道标签映射为实际协议信号名（CH0 -> CH0=SCK）。"""
        nch = min(self.trace.nchannels, 8)
        names = [f"CH{ch}" for ch in range(nch)]
        try:
            if proto == "UART":
                if cfg.tx_ch is not None and cfg.tx_ch < nch:
                    names[cfg.tx_ch] = f"CH{cfg.tx_ch}=TX"
                if cfg.rx_ch is not None and cfg.rx_ch < nch:
                    names[cfg.rx_ch] = f"CH{cfg.rx_ch}=RX"
            elif proto == "I2C":
                if cfg.scl_ch < nch:
                    names[cfg.scl_ch] = f"CH{cfg.scl_ch}=SCL"
                if cfg.sda_ch < nch:
                    names[cfg.sda_ch] = f"CH{cfg.sda_ch}=SDA"
            elif proto == "SPI":
                if cfg.clk_ch < nch:
                    names[cfg.clk_ch] = f"CH{cfg.clk_ch}=SCK"
                if cfg.mosi_ch is not None and cfg.mosi_ch < nch:
                    names[cfg.mosi_ch] = f"CH{cfg.mosi_ch}=MOSI"
                if cfg.miso_ch is not None and cfg.miso_ch < nch:
                    names[cfg.miso_ch] = f"CH{cfg.miso_ch}=MISO"
                if cfg.cs_ch is not None and cfg.cs_ch < nch:
                    names[cfg.cs_ch] = f"CH{cfg.cs_ch}=CS"
        except Exception:  # noqa: BLE001
            pass
        return names

    # ---------------- 视图联动 ----------------
    def _on_sample(self, idx: int):
        self.detail_panel.show_sample(idx)

    def _on_region(self, a: int, b: int):
        """区间拖动：50ms 防抖合并刷新，拖动过程不卡顿。"""
        self._pending_region = (a, b)
        self._region_timer.start()

    def _region_flush(self):
        if self.trace is None or self._pending_region is None:
            return
        a, b = self._pending_region
        self._pending_region = None
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

    def _on_wave_double_click(self, idx: int):
        """双击波形：自动定位最近协议帧并框选，位视图同步刷新。"""
        if self.trace is None or self._last_decode is None:
            return
        proto, cfg = self._last_decode
        try:
            if proto == "UART":
                pkts = dec.decode_uart(self.trace.samples, self.trace.rate, cfg)
            elif proto == "SPI":
                pkts = dec.decode_spi(self.trace.samples, self.trace.rate, cfg)
            else:
                pkts = []
        except Exception:  # noqa: BLE001
            pkts = []
        best = next((p for p in pkts if p.start <= idx < p.end), None)
        if best is None and pkts:
            best = min(pkts, key=lambda p: abs((p.start + p.end) // 2 - idx))
        if best is not None:
            self.wave.set_region_samples(best.start, best.end)
            self.detail_panel.show_range(best.start, best.end)
            self.statusBar().showMessage(
                f"已框选帧 [{best.start} ~ {best.end}] "
                f"({(best.end - best.start) / self.trace.rate * 1e6:.1f} µs)")

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
            f.write("index,time_us,"
                    + ",".join(f"ch{i}" for i in range(self.trace.nchannels))
                    + "\n")
            t_us = np.arange(self.trace.count) / rate * 1e6
            for i in range(0, self.trace.count, 1):
                s = self.trace.samples[i]
                f.write(f"{i},{t_us[i]:.3f},"
                        + ",".join(str((s >> ch) & 1)
                                   for ch in range(self.trace.nchannels))
                        + "\n")
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
