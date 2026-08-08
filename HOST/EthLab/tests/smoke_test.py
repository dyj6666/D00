# -*- coding: utf-8 -*-
"""EthLab 离线冒烟测试（无需板子/网络）。

运行:  python tests/smoke_test.py
验证:  协议解码(ARP/UDP/TCP/ICMP)、校验和、主窗口抓帧链路、保存/导出。
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from eth import decoders          # noqa: E402
from eth.decoders import csum     # noqa: E402


def build_eth(etype, payload, src_mac=b"\x02\x00\x11\x22\x33\x44",
              dst_mac=b"\xff\xff\xff\xff\xff\xff"):
    return dst_mac + src_mac + etype.to_bytes(2, "big") + payload


def build_arp_request():
    p = (b"\x00\x01" + b"\x08\x00" + b"\x06\x04" + b"\x00\x01" +
         b"\x02\x00\x11\x22\x33\x44" + bytes([192, 168, 1, 1]) +
         b"\x00\x00\x00\x00\x00\x00" + bytes([192, 168, 1, 10]))
    return build_eth(0x0806, p)


def build_udp(src_ip, dst_ip, sport, dport, payload):
    udp = (sport.to_bytes(2, "big") + dport.to_bytes(2, "big") +
           (8 + len(payload)).to_bytes(2, "big") + b"\x00\x00" + payload)
    ip = (b"\x45\x00" + (20 + len(udp)).to_bytes(2, "big") +
          b"\x00\x01\x00\x00" + b"\x40\x11" + b"\x00\x00" +
          bytes(int(x) for x in src_ip.split(".")) +
          bytes(int(x) for x in dst_ip.split(".")))
    ip = ip[:10] + csum(ip).to_bytes(2, "big") + ip[12:]
    return build_eth(0x0800, ip + udp)


def build_tcp_syn():
    tcp = (b"\x04\xd2" + b"\x23\x28" + (100).to_bytes(4, "big") +
           (0).to_bytes(4, "big") + b"\x50\x02" + b"\x20\x00" +
           b"\x00\x00" + b"\x00\x00")
    src = bytes([192, 168, 1, 10])
    dst = bytes([192, 168, 1, 1])
    pseudo = src + dst + b"\x00\x06" + len(tcp).to_bytes(2, "big")
    seg = tcp[:16] + b"\x00\x00" + tcp[18:]
    tcp = tcp[:16] + csum(pseudo + seg).to_bytes(2, "big") + tcp[18:]
    ip = (b"\x45\x00" + (20 + len(tcp)).to_bytes(2, "big") +
          b"\x00\x02\x40\x00" + b"\x40\x06" + b"\x00\x00" + src + dst)
    ip = ip[:10] + csum(ip).to_bytes(2, "big") + ip[12:]
    return build_eth(0x0800, ip + tcp)


def build_icmp_echo():
    icmp = (b"\x08\x00" + b"\x00\x00" + b"\x12\x34" + b"\x00\x01" +
            b"A" * 16)
    icmp = icmp[:2] + csum(icmp).to_bytes(2, "big") + icmp[4:]
    src = bytes([192, 168, 1, 10])
    dst = bytes([192, 168, 1, 1])
    ip = (b"\x45\x00" + (20 + len(icmp)).to_bytes(2, "big") +
          b"\x00\x03\x00\x00" + b"\x40\x01" + b"\x00\x00" + src + dst)
    ip = ip[:10] + csum(ip).to_bytes(2, "big") + ip[12:]
    return build_eth(0x0800, ip + icmp)


def test_decoders():
    frames = [
        ("ARP", build_arp_request(), "ARP"),
        ("UDP", build_udp("192.168.1.10", "192.168.1.1", 7777, 7777,
                          b"hello"), "UDP"),
        ("TCP", build_tcp_syn(), "TCP"),
        ("ICMP", build_icmp_echo(), "ICMP"),
    ]
    for name, raw, proto in frames:
        fr = decoders.decode("TX", raw)
        got = fr.l4 if fr.l4 else fr.proto
        assert got == proto, (name, got)
        checks = [f for f in fr.fields if f.name in
                  ("头部校验和", "校验和")]
        for f in checks:
            assert "FAIL" not in f.desc, (name, f.name, f.desc)
        print("decoder OK:", name, fr.summary)
    print("ALL DECODERS PASS")


def test_gui():
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from PyQt5.QtWidgets import QApplication
    from ui.main_window import MainWindow
    from ui.capture_panel import parse_capture_datagram

    app = QApplication([])
    win = MainWindow()

    raw_arp = build_arp_request()
    raw_udp = build_udp("192.168.1.10", "192.168.1.1", 7777, 7777, b"Hi")
    win.capture.add_frame("RX", raw_arp)
    win.capture.add_frame("TX", raw_udp)
    assert win.capture.count() == 2
    assert win.capture.table.rowCount() == 2, win.capture.table.rowCount()

    # 截断帧：orig_len > raw
    win.capture.add_frame("RX", raw_udp[:60], orig_len=1518, truncated=True)
    assert win.capture.count() == 3

    # 选择第一行 -> 帧详情更新
    win.capture.table.selectRow(0)
    assert win.detail._frame is not None
    assert win.detail.byte_view._frame is not None

    # 保存/加载/导出
    tmp = tempfile.mkdtemp(prefix="ethlab_test_")
    jp = os.path.join(tmp, "cap.json")
    assert win.capture.save_json(jp) == 3
    win.capture.clear()
    assert win.capture.count() == 0
    assert win.capture.load_json(jp) == 3
    pp = os.path.join(tmp, "cap.pcap")
    assert win.capture.export_pcap(pp) == 3
    assert os.path.getsize(pp) > 24 + 3 * 16

    # 抓帧载荷解析
    pkt = b"\x02\x00" + (len(raw_arp)).to_bytes(2, "big") + raw_arp
    parsed = parse_capture_datagram(pkt)
    assert parsed[0] == "RX" and parsed[1] == raw_arp

    win._listener.stop()
    print("GUI SMOKE PASS")


if __name__ == "__main__":
    test_decoders()
    test_gui()
    print("ALL SMOKE TESTS PASS")
