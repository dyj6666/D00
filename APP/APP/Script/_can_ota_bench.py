#!/usr/bin/env python3
"""CAN OTA 速率基准（不入库）：不同发送时序/窗口下的稳定吞吐测量。

用法：python _can_ota_bench.py [inter_frame_us] [window] [blocks]
退出前自动 ABORT，不触发 BOOT 切换；需要设备端 OTA-CAN 服务在线。
"""

import os
import struct
import sys
import time

sys.path.insert(0, r"D:\GIT-SPACE\D00\HOST\D00Term")
import d00term as dt  # noqa: E402

sys.path.insert(0, r"D:\GIT-SPACE\D00\HOST\OTA_Tool")
from core.ymodem_sender import encrypt_and_sign, derive_aes_key_from_uid  # noqa: E402

CAN_OTA_CTRL_ID = 0x200
CAN_OTA_DATA_ID = 0x201
CAN_OTA_REPLY_ID = 0x210
CAN_OTA_ACK_ID = 0x211

CMD_BEGIN, CMD_END, CMD_STATUS, CMD_ABORT = 0x01, 0x02, 0x03, 0x04
REP_BEGIN_OK = 0x81
ACK_OK, ACK_ERR = 0x81, 0x82
OTA_BLOCK = 240


def main():
    sleep_us = float(sys.argv[1]) if len(sys.argv) > 1 else 150.0
    window = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    blocks = int(sys.argv[3]) if len(sys.argv) > 3 else 60

    # 造一个合法的加密签名包（内容为伪随机，仅用于速率基准）
    here = os.path.dirname(os.path.abspath(__file__))
    raw = os.path.join(here, "_bench_raw.bin")
    pkg = os.path.join(here, "_bench_pkg.bin")
    import json
    with open(os.path.join(here, r"..\..\..\HOST\OTA_Tool\local_keys.json"),
              encoding="utf-8") as f:
        sec = json.load(f)
    blob = bytes((i * 131 + 7) & 0xFF for i in range(blocks * OTA_BLOCK))
    with open(raw, "wb") as f:
        f.write(blob)
    aes = derive_aes_key_from_uid(sec["device_uid"])
    encrypt_and_sign(raw, pkg, sec["private_key"], aes.hex(), 202, 0x413, 999)
    data = open(pkg, "rb").read()
    size = len(data)

    api = dt._PcanApi(dt._load_pcan_dll())
    if api.initialize() != dt._PCAN_ERROR_OK:
        print("CAN init FAIL")
        return 1

    def send(mid, payload):
        msg = dt._PcanMsg()
        msg.id = mid
        msg.msgtype = dt._PCAN_MESSAGE_STANDARD
        msg.len = len(payload)
        for i, b in enumerate(payload):
            msg.data[i] = b
        while api.write(msg) != dt._PCAN_ERROR_OK:
            time.sleep(0.0001)   # PCAN 发送缓冲满：等待后重试（不丢帧）

    def recv_ack(timeout=1.0):
        end = time.time() + timeout
        while time.time() < end:
            r = api.read_raw()
            if r is None:
                time.sleep(0.0002)
                continue
            mid, d = r
            if mid == CAN_OTA_ACK_ID and d and d[0] == ACK_OK:
                return struct.unpack_from("<I", d, 1)[0]
        return None

    send(CAN_OTA_CTRL_ID, bytes([CMD_BEGIN]) + struct.pack("<IHB", size, 202, 0))
    # BEGIN 应答在 0x210，不是 0x211：单独等
    end = time.time() + 3.0
    begin_ok = False
    while time.time() < end:
        r = api.read_raw()
        if r is None:
            time.sleep(0.0002)
            continue
        mid, d = r
        if mid == CAN_OTA_REPLY_ID and d and d[0] == REP_BEGIN_OK:
            begin_ok = True
            break
    if not begin_ok:
        print("BEGIN FAIL")
        api.uninitialize()
        return 1

    t0 = time.time()
    sent = 0
    acked = 0
    total_blocks = (size + OTA_BLOCK - 1) // OTA_BLOCK
    while sent < size:
        # 窗口背压：在途块数达上限时先等 ACK
        while window > 0 and (sent - acked) >= window * OTA_BLOCK:
            a = recv_ack()
            if a is None:
                print("ACK timeout, aborting bench")
                send(CAN_OTA_CTRL_ID, bytes([CMD_ABORT]))
                api.uninitialize()
                return 1
            acked = max(acked, a)
        chunk = data[sent:sent + OTA_BLOCK]
        for frame in dt.encode_can_line(chunk):
            send(CAN_OTA_DATA_ID, frame)
            if sleep_us > 0:
                time.sleep(sleep_us / 1e6)
        sent += len(chunk)
    while acked < size:
        a = recv_ack(5.0)
        if a is None:
            print("tail ACK timeout")
            break
        acked = max(acked, a)
    dt_sec = time.time() - t0
    print(f"BENCH sleep={sleep_us:.0f}us window={window} blocks={total_blocks} "
          f"size={size}B time={dt_sec:.2f}s rate={size/dt_sec/1024:.1f} KB/s")

    send(CAN_OTA_CTRL_ID, bytes([CMD_ABORT]))
    time.sleep(0.2)
    api.uninitialize()
    return 0


if __name__ == "__main__":
    sys.exit(main())
