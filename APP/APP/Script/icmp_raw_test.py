#!/usr/bin/env python3
"""原始套接字 ICMP 测试（绕过 Windows ping.exe / ICMP 状态机）

用法（管理员 PowerShell）：
    python D:\GIT-SPACE\D00\APP\APP\Script\icmp_raw_test.py [目标IP]

如果收到板子的 ICMP 回复 → 板端 ICMP 100% 正常，问题在 Windows ICMP 模块；
如果超时 → 帧没到网卡/驱动层，另查。
"""

import random
import socket
import struct
import sys
import time


def csum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\x00"
    s = sum(((data[i] << 8) | data[i + 1]) for i in range(0, len(data), 2))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def main() -> int:
    target = sys.argv[1] if len(sys.argv) > 1 else "192.168.10.10"
    icmp_id = random.randint(0, 0xFFFF)
    seq = 1
    payload = b"D00-ICMP-RAW-TEST"

    pkt = struct.pack("!BBHHH", 8, 0, 0, icmp_id, seq) + payload
    pkt = pkt[:2] + struct.pack("!H", csum(pkt)) + pkt[4:]

    s = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)
    s.settimeout(3)
    t0 = time.perf_counter()
    s.sendto(pkt, (target, 0))
    print(f"sent ICMP echo request -> {target}  id=0x{icmp_id:04X} seq={seq}")
    try:
        data, _ = s.recvfrom(4096)
        dt = (time.perf_counter() - t0) * 1000
        ip_ihl = (data[0] & 0x0F) * 4
        typ, code, _chk, rid, rseq = struct.unpack("!BBHHH", data[ip_ihl:ip_ihl + 8])
        print(f"reply: type={typ} code={code} id=0x{rid:04X} seq={rseq} RTT={dt:.2f} ms")
        if typ == 0 and rid == icmp_id and rseq == seq:
            print("RESULT: MCU ICMP OK - Windows ICMP stack is the problem")
        else:
            print("RESULT: reply fields mismatch - check board side")
        return 0
    except socket.timeout:
        print("RESULT: timeout - no ICMP reply received")
        return 1
    finally:
        s.close()


if __name__ == "__main__":
    raise SystemExit(main())
