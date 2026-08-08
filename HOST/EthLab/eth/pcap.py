# -*- coding: utf-8 -*-
"""PCAP 导出（Wireshark 直接打开）。"""

import struct
import time

PCAP_MAGIC = 0xA1B2C3D4
LINKTYPE_ETHERNET = 1


def write_pcap(path, frames):
    """frames: 可迭代的 eth.decoders.Frame 对象。"""
    with open(path, "wb") as f:
        f.write(struct.pack("<IHHiIII", PCAP_MAGIC, 2, 4, 0, 0, 65535,
                            LINKTYPE_ETHERNET))
        for fr in frames:
            raw = fr.raw
            ts = fr.ts or time.time()
            sec = int(ts)
            usec = int((ts - sec) * 1_000_000)
            f.write(struct.pack("<IIII", sec, usec, len(raw), len(raw)))
            f.write(raw)
