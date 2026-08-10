#!/usr/bin/env python3
"""以太网 TCP OTA 命令行工具（板端 ota_tcp_svc :9020）。

用法：
    python ota_tcp_cli.py [APP.bin] [version] [build_no] [ip] [--no-resume]

与 ota_hostlink_cli 同构：构建加密签名包 -> BEGIN -> DATA(分块) -> END，
板端 Ota_Begin/Data/End 复用同一下载核心，BOOT 安全校验不变。
"""
import os
import sys
import time
import struct
import socket
import socket as _socket

HOST_TOOL = r"D:\GIT-SPACE\D00\HOST\OTA_Tool"
sys.path.insert(0, HOST_TOOL)

from core.ymodem_sender import encrypt_and_sign, derive_aes_key_from_uid  # noqa: E402

LEGACY_PRIVKEY = ("53360076d1539e52f9cd5cb9f1ca5076"
                  "ea5270df32b50003a6eaa16559245106")

OTA_CHUNK_MAX = 240
MAGIC = 0x5A
CMD_BEGIN, CMD_DATA, CMD_END, CMD_STATUS, CMD_RESET = 1, 2, 3, 4, 5


def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def frame(cmd, payload=b""):
    hdr = bytes([MAGIC, cmd, (len(payload) >> 8) & 0xFF, len(payload) & 0xFF])
    return hdr + payload + bytes([crc8(hdr[1:] + payload)])


def recv_exact(s, n, timeout):
    s.settimeout(timeout)
    data = b""
    while len(data) < n:
        c = s.recv(n - len(data))
        if not c:
            return None
        data += c
    return data


def cmd(s, c, payload=b"", timeout=2.0):
    s.settimeout(timeout)
    s.sendall(frame(c, payload))
    hdr = recv_exact(s, 4, timeout)
    if hdr is None or hdr[0] != MAGIC or hdr[1] != 0x80:
        return None
    plen = (hdr[2] << 8) | hdr[3]
    body = recv_exact(s, plen + 1, timeout)
    if body is None:
        return None
    if body[-1] != crc8(hdr[1:] + body[:-1]):
        return None
    return body[:-1]


def main():
    raw = sys.argv[1:]
    no_resume = "--no-resume" in raw
    args = [a for a in raw if a != "--no-resume"]
    app_bin = args[0] if args else r"D:\GIT-SPACE\D00\APP\APP\MDK-ARM\Output\APP.bin"
    version = int(args[1]) if len(args) > 1 else 192
    build_no = int(args[2]) if len(args) > 2 else 0
    ip = args[3] if len(args) > 3 else "192.168.10.10"
    port = 9020
    uid = "002900363132470830323031"
    privkey = os.environ.get("OTA_PRIVKEY", "").strip() or LEGACY_PRIVKEY

    pkg_path = os.path.join(os.path.dirname(app_bin), "_ota_v76.bin")
    aes_key = derive_aes_key_from_uid(uid)
    encrypt_and_sign(app_bin, pkg_path, privkey, aes_key.hex(),
                     version, 0x413, build_no)
    with open(pkg_path, "rb") as f:
        pkg = f.read()
    print(f"[OTA-TCP] file={app_bin} ver={version} build={build_no} "
          f"pkg={len(pkg)}B -> {ip}:{port}")

    s = socket.create_connection((ip, port), timeout=5)
    s.setsockopt(_socket.IPPROTO_TCP, _socket.TCP_NODELAY, 1)
    try:
        if no_resume:
            print("[OTA-TCP] reset session ...")
            cmd(s, CMD_RESET, timeout=3.0)
            time.sleep(0.2)

        # BEGIN 内 Ota_Begin 会同步擦除 2x128KB 下载扇区（约 2-4s），超时需放宽
        r = cmd(s, CMD_BEGIN, struct.pack(">II", version, len(pkg)),
                timeout=15.0)
        if r is None or r[0] != 0:
            print(f"[OTA-TCP] BEGIN FAILED: {r}")
            return 1
        print("[OTA-TCP] BEGIN OK")

        start_off = 0
        if not no_resume:
            r = cmd(s, CMD_STATUS)
            if r is not None and len(r) >= 9 and r[0] == 1:
                rx = int.from_bytes(r[1:5], "big")
                if 0 < rx < len(pkg):
                    start_off = rx
                    print(f"[OTA-TCP] resume from {rx}")

        for off in range(start_off, len(pkg), OTA_CHUNK_MAX):
            blk = pkg[off:off + OTA_CHUNK_MAX]
            r = cmd(s, CMD_DATA, struct.pack(">I", off) + blk)
            if r is None:
                print(f"[OTA-TCP] DATA {off} no response")
                return 1
            if r[0] != 0:
                print(f"[OTA-TCP] DATA {off} state={r[0]}")
                return 1
            rx = int.from_bytes(r[1:5], "big")
            if off % 4800 == 0:
                print(f"[OTA-TCP] downloaded {rx}/{len(pkg)}")

        r = cmd(s, CMD_STATUS)
        if r is not None and len(r) >= 9:
            print(f"[OTA-TCP] STATUS state={r[0]} "
                  f"{int.from_bytes(r[1:5],'big')}/{int.from_bytes(r[5:9],'big')}")

        print("[OTA-TCP] send END, trigger BOOT ...")
        # Ota_End 校验完成后立即复位 MCU（不回 ACK），超时/断连视为成功
        try:
            r = cmd(s, CMD_END, timeout=1.0)
            if r is not None and r[0] != 0:
                print(f"[OTA-TCP] END FAILED: {r[0]}")
                return 1
        except (TimeoutError, socket.timeout, ConnectionResetError):
            pass
    finally:
        s.close()
    print("[OTA-TCP] done (check BOOT/APP boot log)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
