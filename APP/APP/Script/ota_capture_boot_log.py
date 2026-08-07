#!/usr/bin/env python3
"""完整 OTA 下载 + 触发 BOOT 校验，并抓取 BOOT 调试串口（COM9）日志。"""

import sys
import time
import threading
import struct

import serial

sys.path.insert(0, r"D:\GIT-SPACE\D00\HOST\OTA_Tool")
from core.hostlink import (            # noqa: E402
    FrameParser, OTA_CHUNK_MAX, build_ota_begin, build_ota_status,
    build_ota_data, build_ota_end, CMD_OTA_BOOT_STATUS,
)
from core.ymodem_sender import encrypt_and_sign, derive_aes_key_from_uid  # noqa: E402

LEGACY_PRIVKEY = ("53360076d1539e52f9cd5cb9f1ca5076"
                  "ea5270df32b50003a6eaa16559245106")

APP_BIN = r"D:\GIT-SPACE\D00\APP\APP\MDK-ARM\Output\APP.bin"
PKG_OUT = r"D:\GIT-SPACE\D00\APP\APP\MDK-ARM\Output\_ota_v76b.bin"
VERSION = 76
BUILD_NO = 57
UID = "002900363132470830323031"

DEBUG_PORT = "COM9"
HOST_PORT = "COM13"

logs = []
stop = threading.Event()


def debug_reader():
    ser = serial.Serial(DEBUG_PORT, 115200, timeout=0.2)
    while not stop.is_set():
        try:
            n = ser.in_waiting
            if n:
                data = ser.read(n)
                logs.append(data.decode("utf-8", errors="replace"))
        except Exception:
            break
    ser.close()


def main() -> int:
    t = threading.Thread(target=debug_reader, daemon=True)
    t.start()

    # 构建安全包（build 57，规避可能的防重放拒绝）
    aes_key = derive_aes_key_from_uid(UID)
    encrypt_and_sign(APP_BIN, PKG_OUT, LEGACY_PRIVKEY, aes_key.hex(),
                     VERSION, 0x413, BUILD_NO)
    pkg = open(PKG_OUT, "rb").read()
    print(f"[CAP] pkg {len(pkg)} bytes, version={VERSION} build={BUILD_NO}")

    ser = serial.Serial(HOST_PORT, 921600, timeout=0.1)
    parser = FrameParser()

    def cmd(frame, expect_cmd, timeout=2.0, retries=3):
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
                                print(f"    [BOOT] phase={pl[0]} err={pl[1]}")
                            continue
                        if fr[2] == expect_cmd:
                            return fr
                time.sleep(0.002)
        return None

    # BEGIN
    r = cmd(build_ota_begin(VERSION, len(pkg)), 0x08)
    if r is None:
        print("[CAP] BEGIN no response")
        ser.close()
        stop.set()
        return 1
    print(f"[CAP] BEGIN st={r[5]}")

    # STATUS：读取会话进度
    r = cmd(build_ota_status(), 0x0B)
    if r is not None:
        st, rx, total = r[5], struct.unpack("<I", r[6:10])[0], \
            struct.unpack("<I", r[10:14])[0]
        print(f"[CAP] STATUS state={st} rx={rx} total={total}")

    # 下载数据
    start_off = 0
    if r is not None:
        st = r[5]
        rx = struct.unpack("<I", r[6:10])[0]
        if st == 1 and 0 < rx < len(pkg):
            start_off = rx
    for off in range(start_off, len(pkg), OTA_CHUNK_MAX):
        blk = pkg[off:off + OTA_CHUNK_MAX]
        rr = cmd(build_ota_data(off, blk), 0x09)
        if rr is None or rr[5] != 0:
            print(f"[CAP] DATA block {off} failed")
            ser.close()
            stop.set()
            return 1
        if off % 4800 == 0:
            rx = struct.unpack("<I", rr[6:10])[0]
            print(f"[CAP] downloaded {rx}/{len(pkg)}")
        time.sleep(0.01)

    # END：触发 BOOT 校验
    print("[CAP] send END ...")
    cmd(build_ota_end(), 0x0A, timeout=1.0, retries=2)

    # 等待 BOOT 完成校验/回退（留出打印时间）
    time.sleep(8)
    ser.close()
    stop.set()
    t.join(timeout=3)

    print("\n===== BOOT/APP debug log (COM9) =====")
    for chunk in logs:
        print(chunk, end="")
    print("\n===== end log =====")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
