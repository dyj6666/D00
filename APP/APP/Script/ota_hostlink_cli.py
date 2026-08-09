#!/usr/bin/env python3
"""HOSTLINK 运行时 OTA 命令行工具（无 GUI 依赖）。

与 HOST/OTA_Tool 的 ota_worker hostlink 模式等价：
  BEGIN -> STATUS(断点续传) -> DATA 分块 -> STATUS -> END -> 监听 BOOT 状态帧

用法:
    python ota_hostlink_cli.py [APP.bin] [version] [build_no] [port]
    python ota_hostlink_cli.py --no-resume ...   # 强制全新下载（默认续传）

安全说明:
    - 私钥仅从环境变量 OTA_PRIVKEY 注入（64 位十六进制 ECDSA 私钥）；
    - 未设置时回退 LEGACY 密钥（BOOT 双公钥兼容，仅用于开发验证）。
"""

import os
import sys
import time
import struct

import serial

HOST_TOOL = r"D:\GIT-SPACE\D00\HOST\OTA_Tool"
sys.path.insert(0, HOST_TOOL)

from core.hostlink import (           # noqa: E402
    FrameParser, OTA_CHUNK_MAX,
    build_ota_begin, build_ota_data,
    build_ota_end, build_ota_status, build_ota_reset,
    CMD_OTA_BOOT_STATUS,
)
from core.ymodem_sender import encrypt_and_sign, derive_aes_key_from_uid  # noqa: E402

# ---- LEGACY 开发验证密钥（与 BOOT 内 ECDSA_PUB_KEY_LEGACY 配对）----
LEGACY_PRIVKEY = ("53360076d1539e52f9cd5cb9f1ca5076"
                  "ea5270df32b50003a6eaa16559245106")


def main() -> int:
    raw_args = sys.argv[1:]
    no_resume = "--no-resume" in raw_args
    args = [a for a in raw_args if a != "--no-resume"]
    app_bin = args[0] if args else r"D:\GIT-SPACE\D00\APP\APP\MDK-ARM\Output\APP.bin"
    version = int(args[1]) if len(args) > 1 else 76
    build_no = int(args[2]) if len(args) > 2 else 56
    port = args[3] if len(args) > 3 else "COM13"
    baud = 921600
    uid = "002900363132470830323031"          # 同板（config.json 最近会话）
    privkey = os.environ.get("OTA_PRIVKEY", "").strip() or LEGACY_PRIVKEY

    print(f"[OTA] file={app_bin}")
    print(f"[OTA] version={version} build={build_no} port={port} baud={baud}")
    print(f"[OTA] key={'ENV' if os.environ.get('OTA_PRIVKEY') else 'LEGACY'}")

    pkg_path = os.path.join(os.path.dirname(app_bin), "_ota_v76.bin")
    aes_key = derive_aes_key_from_uid(uid)
    encrypt_and_sign(app_bin, pkg_path, privkey, aes_key.hex(),
                     version, 0x413, build_no)
    with open(pkg_path, "rb") as f:
        pkg = f.read()
    print(f"[OTA] secure pkg {len(pkg)} bytes -> {pkg_path}")

    ser = serial.Serial(port, baud, timeout=0.1)
    parser = FrameParser()

    def cmd(frame, expect_cmd, timeout=1.0, retries=4):
        # 不重置输入缓冲：避免清掉延迟到达的响应（解析器自带重同步）
        ser.write(frame)
        parser._buf.clear()
        for _ in range(retries):
            t0 = time.time()
            while time.time() - t0 < timeout:
                n = ser.in_waiting
                d = ser.read(n) if n else b""
                if d:
                    parser.feed(d)
                    for fr in parser.frames():
                        if fr[2] == CMD_OTA_BOOT_STATUS:
                            pl = FrameParser.payload(fr)
                            if len(pl) >= 2:
                                phase, err = pl[0], pl[1]
                                print(f"    [BOOT] phase={phase} err={err}")
                            continue
                        if fr[2] == expect_cmd:
                            return fr
                time.sleep(0.002)
        return None

    # 0) 强制全新下载：先复位设备端 OTA 会话（旧版本固件无此命令，忽略即可）
    if no_resume:
        print("[OTA] reset device OTA session ...")
        cmd(build_ota_reset(), 0x0D, timeout=0.8, retries=2)
        time.sleep(0.2)

    # 1) BEGIN
    print("[OTA] send BEGIN ...")
    r = cmd(build_ota_begin(version, len(pkg)), 0x08)
    if r is None or r[5] != 0:
        print(f"[OTA] BEGIN FAILED: {r[5] if r else 'no response'}")
        ser.close()
        return 1
    print("[OTA] BEGIN OK, download area ready")

    # 2) 断点续传查询
    start_off = 0
    if not no_resume:
        r = cmd(build_ota_status(), 0x0B)
        if r is not None:
            st = r[5]
            rx, total = struct.unpack("<II", r[6:14])
            if st == 1 and 0 < rx < len(pkg):
                start_off = rx
                print(f"[OTA] resume from {rx} bytes")
    else:
        print("[OTA] fresh download (--no-resume)")

    # 3) DATA 分块
    for off in range(start_off, len(pkg), OTA_CHUNK_MAX):
        blk = pkg[off:off + OTA_CHUNK_MAX]
        r = cmd(build_ota_data(off, blk), 0x09)
        if r is None:
            print(f"[OTA] DATA block {off} no response")
            ser.close()
            return 1
        if r[5] != 0:
            print(f"[OTA] DATA block {off} state={r[5]}")
            ser.close()
            return 1
        rx = struct.unpack("<I", r[6:10])[0]
        if off % 2400 == 0:
            print(f"[OTA] downloaded {rx}/{len(pkg)} bytes")
        time.sleep(0.01)

    # 4) STATUS 确认
    r = cmd(build_ota_status(), 0x0B)
    if r:
        st, rx, total = r[5], struct.unpack("<I", r[6:10])[0], \
            struct.unpack("<I", r[10:14])[0]
        print(f"[OTA] STATUS: state={st} {rx}/{total}")

    # 5) END 触发 BOOT 切换
    print("[OTA] send END, trigger BOOT switch ...")
    r = cmd(build_ota_end(), 0x0A, timeout=1.0, retries=2)
    if r is not None and r[5] != 0:
        print(f"[OTA] END FAILED: {r[5]}")
        ser.close()
        return 1
    print("[OTA] END triggered, device resetting ...")

    # 6) 监听 BOOT 状态帧（12s）
    t0 = time.time()
    parser2 = FrameParser()
    while time.time() - t0 < 12.0:
        n = ser.in_waiting
        if n:
            d = ser.read(n)
            if d:
                parser2.feed(d)
                for fr in parser2.frames():
                    if fr[2] == CMD_OTA_BOOT_STATUS:
                        pl = FrameParser.payload(fr)
                        if len(pl) >= 2:
                            phase, err = pl[0], pl[1]
                            print(f"    [BOOT] phase={phase} err={err}")
        time.sleep(0.01)

    ser.close()
    print("[OTA] done (check BOOT/APP boot log for final state)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
