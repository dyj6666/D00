#!/usr/bin/env python3
"""本机最小 DNS 服务器 + 转发器（供直连无外网的开发板做域名解析）。

背景：开发板与 PC 直连（无网关），板载 DNS 客户端无法访问公网 DNS。
本脚本在 PC 上监听 UDP 53：本地已知域名直接应答（A 记录），未知域名
转发到公网 DNS（默认 223.5.5.5）后原样回传。

用法：
    python dns_server.py [port]      # 默认 53
板端：`dns server 192.168.10.201`，然后 `dns resolve <host>`。
"""

import socket
import struct
import sys
import threading

LOCAL_RECORDS = {
    b"d00.test.": "192.168.10.10",
    b"pc.test.": "192.168.10.201",
    b"ntp.test.": "192.168.10.201",
    b"ota.test.": "192.168.10.201",
}
FORWARD_DNS = ("223.5.5.5", 53)


def parse_name(data, off):
    """解析 DNS 名字（支持压缩指针），返回 (name, 新偏移)。"""
    labels = []
    jumped = 0
    while True:
        if off >= len(data):
            raise ValueError("name out of range")
        ln = data[off]
        if ln == 0:
            off += 1
            break
        if ln & 0xC0 == 0xC0:
            ptr = ((ln & 0x3F) << 8) | data[off + 1]
            if jumped == 0:
                off += 2
            sub, _ = parse_name(data, ptr)
            labels.append(sub)
            jumped = 1
            break
        if ln & 0xC0 != 0:
            raise ValueError("bad label")
        off += 1
        labels.append(data[off:off + ln].decode("latin1"))
        off += ln
    name = ".".join(labels) + "."
    return name.encode("latin1"), off


def build_response(query, qname, qtype, ip):
    """构造 DNS 应答：原样回显问题段 + A 记录回答段。"""
    resp = bytearray(query[:2])              # ID
    resp += b"\x81\x80"                      # flags: QR=1, RD=1, RA=1
    resp += b"\x00\x01"                      # QDCOUNT=1
    resp += b"\x00\x01"                      # ANCOUNT=1
    resp += b"\x00\x00\x00\x00"              # NS/AR
    # 问题段原样回显
    qstart = 12
    while query[qstart] != 0:
        qstart += query[qstart] + 1
    resp += query[12:qstart + 1]
    resp += query[qstart + 1:qstart + 5]
    # 回答段：A 记录
    resp += b"\xc0\x0c"                      # 名字指针 -> 问题段
    resp += struct.pack(">HHIH", 1, 1, 60, 4)  # TYPE=A CLASS=IN TTL=60 RDLEN=4
    resp += socket.inet_aton(ip)
    return bytes(resp)


def handle(data, addr, sock):
    try:
        if len(data) < 12 or struct.unpack("!H", data[4:6])[0] == 0:
            return
        qname, off = parse_name(data, 12)
        qtype = struct.unpack("!H", data[off:off + 2])[0]
        if qtype != 1:                        # 只处理 A 查询，其余转发
            raise ValueError("non-A query")
        ip = LOCAL_RECORDS.get(qname)
        if ip:
            sock.sendto(build_response(data, qname, qtype, ip), addr)
            print(f"[DNS] {qname.decode()} -> {ip} (local)")
            return
        # 未知域名：转发公网 DNS，回传原始响应
        fwd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        fwd.settimeout(3.0)
        fwd.sendto(data, FORWARD_DNS)
        rdata, _ = fwd.recvfrom(4096)
        sock.sendto(rdata, addr)
        print(f"[DNS] {qname.decode()} -> forwarded via {FORWARD_DNS[0]}")
        fwd.close()
    except Exception as e:
        print(f"[DNS] query from {addr} error: {e}")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 53
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    print(f"[DNS] listening on udp 0.0.0.0:{port} (Ctrl+C 退出)")
    while True:
        data, addr = sock.recvfrom(4096)
        threading.Thread(target=handle, args=(data, addr, sock), daemon=True).start()


if __name__ == "__main__":
    main()
