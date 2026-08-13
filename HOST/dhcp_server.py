#!/usr/bin/env python3
"""本机最小 DHCP 服务器（供直连无外网的开发板验证动态获取 IP）。

背景：开发板与 PC 直连，默认静态 IP；为验证板端 DHCP 客户端，本脚本
在 PC 上监听 UDP 67，给指定 MAC 分配固定地址（默认与静态 IP 相同，
拓扑不变化）。15s 内未完成租约则板端自动回退静态配置。

用法：
    python dhcp_server.py [client_ip] [client_mac] [bind_ip] [port]
板端：`dhcp on`，验证后 `dhcp off` 回静态。
"""

import socket
import struct
import sys

LEASE_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.10.10"
CLIENT_MAC = sys.argv[2] if len(sys.argv) > 2 else "020011223344"
BIND_IP = sys.argv[3] if len(sys.argv) > 3 else "192.168.10.201"
PORT = int(sys.argv[4]) if len(sys.argv) > 4 else 67
SERVER_IP = "192.168.10.201"
NETMASK = "255.255.255.0"
GATEWAY = SERVER_IP
LEASE_TIME = 3600
IP = lambda s: socket.inet_aton(s)  # noqa: E731


def dhcp_msgtype(options):
    """从 DHCP 选项区取消息类型（53）。"""
    i = 0
    while i < len(options):
        if options[i] == 255:
            break
        if options[i] == 0:
            i += 1
            continue
        ln = options[i + 1]
        if options[i] == 53 and ln >= 1:
            return options[i + 2]
        i += 2 + ln
    return None


def build_reply(pkt, msg_type):
    """构造 DHCP OFFER(2)/ACK(5)：回显事务 ID 与 chaddr。"""
    if len(pkt) < 240:
        return None
    xid = pkt[4:8]
    flags = pkt[10:12]
    chaddr = pkt[28:44]
    opts = bytearray()
    opts += bytes([53, 1, msg_type])               # DHCP message type
    opts += bytes([1, 4]) + IP(NETMASK)            # subnet mask
    opts += bytes([3, 4]) + IP(GATEWAY)            # router
    opts += bytes([51, 4]) + struct.pack("!I", LEASE_TIME)  # lease time
    opts += bytes([54, 4]) + IP(SERVER_IP)         # server identifier
    opts += b"\xff"
    reply = bytearray(240)
    reply[0] = 2                                   # BOOTREPLY
    reply[1] = 1                                   # Ethernet
    reply[2] = 6
    reply[3] = 0                                   # hops
    reply[4:8] = xid
    reply[8:10] = b"\x00\x00"
    reply[10:12] = flags
    reply[12:16] = IP("0.0.0.0")
    reply[16:20] = IP(LEASE_IP)                    # yiaddr
    reply[20:24] = IP(SERVER_IP)                   # siaddr
    reply[24:28] = IP("0.0.0.0")
    reply[28:44] = chaddr
    reply[44:108] = b"\x00" * 64                   # sname/bootfile 留空
    reply[108:236] = b"\x00" * 128
    reply[236:240] = b"\x63\x82\x53\x63"           # DHCP magic cookie
    reply += opts
    return bytes(reply)


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    # 绑定指定网卡 IP：多网卡（WiFi+直连）下确保广播从直连口发出
    sock.bind((BIND_IP, PORT))
    print(f"[DHCP] listening udp {BIND_IP}:{PORT} -> {LEASE_IP} for MAC {CLIENT_MAC}")
    while True:
        data, addr = sock.recvfrom(1024)
        try:
            if len(data) < 240 or data[0] != 1:    # 只收 BOOTREQUEST
                continue
            ch = data[28:34].hex()
            if ch.lower() != CLIENT_MAC.lower():
                print(f"[DHCP] ignore {ch} (not target)")
                continue
            opts = data[240:]
            mt = dhcp_msgtype(opts)
            if mt == 1:
                rep = build_reply(data, 2)         # DISCOVER -> OFFER
                sock.sendto(rep, ("255.255.255.255", 68))
                print(f"[DHCP] OFFER {LEASE_IP} -> {ch}")
            elif mt == 3:
                rep = build_reply(data, 5)         # REQUEST -> ACK
                sock.sendto(rep, ("255.255.255.255", 68))
                print(f"[DHCP] ACK {LEASE_IP} -> {ch}")
        except Exception as e:
            print(f"[DHCP] error: {e}")


if __name__ == "__main__":
    main()
