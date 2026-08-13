#!/usr/bin/env python3
"""本机最小 NTP 服务器（供无外网的开发板做时间校准）。

背景：开发板与 PC 直连（无网关/无外网），板载 SNTP 客户端无法直接访问
公网 NTP。本脚本在 PC 上监听 UDP 123，把 PC 的系统时间（已由系统/WiFi
同步）以 NTP 协议回给开发板，板端执行 `sntp sync 192.168.10.201` 即可。

用法：
    python ntp_server.py [port]      # 默认 123
"""

import socket
import struct
import sys
import time

NTP_EPOCH_DELTA = 2208988800  # 1900 -> 1970 秒差


def unix_to_ntp(ts):
    """Unix 秒 -> NTP 64 位时间戳（秒 + 小数）。"""
    sec = int(ts) + NTP_EPOCH_DELTA
    frac = int((ts - int(ts)) * (1 << 32))
    return sec, frac


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 123
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    print(f"[NTP] listening on udp 0.0.0.0:{port} (Ctrl+C 退出)")
    while True:
        data, addr = sock.recvfrom(48)
        if len(data) < 48:
            continue
        # 客户端发送时间戳（字节 40..43）
        client_tx_sec = struct.unpack("!I", data[40:44])[0]

        now = time.time()
        rx_sec, rx_frac = unix_to_ntp(now)
        tx_sec, tx_frac = unix_to_ntp(now)

        pkt = bytearray(48)
        pkt[0] = 0x1C  # LI=0, VN=3, MODE=4(server)
        pkt[1] = 1     # stratum 1（本机时钟）
        # 原始时间戳 = 客户端发送时间戳
        struct.pack_into("!I", pkt, 24, client_tx_sec)
        # 接收时间戳
        struct.pack_into("!II", pkt, 32, rx_sec, rx_frac)
        # 发送时间戳
        struct.pack_into("!II", pkt, 40, tx_sec, tx_frac)
        sock.sendto(bytes(pkt), addr)
        print(f"[NTP] reply to {addr[0]}:{addr[1]} @ {time.strftime('%H:%M:%S')}")


if __name__ == "__main__":
    main()
