#!/usr/bin/env python3
"""CAN OTA 推送 CLI —— 与固件 ota_can_svc 对称（PCAN-USB @ 1Mbps）。

用法：
    python ota_can_cli.py <APP.bin> <version> [build_no]
    python ota_can_cli.py --status          # 查询 OTA 状态/进度
    python ota_can_cli.py --reset           # 清会话回 IDLE

协议（与 Config/can_proto.h 严格一致）：
    控制 0x200：BEGIN(version+size) / END / STATUS / ABORT；
    数据 0x201：行帧规约（首字节序号+0x80 末帧，负载 ≤7B），每 240B 一组；
    应答 0x210：BEGIN_OK/ERR、END_RESULT、STATUS、ABORT 回执。
注意：必须先把 APP.bin 加密签名为 OTA 包再发送（BOOT 验签），
     不能裸发 APP.bin（否则 BOOT 安全校验拒绝、无法应用）。
"""

import os
import struct
import sys
import time

sys.path.insert(0, r"D:\GIT-SPACE\D00\HOST\D00Term")
import d00term as dt  # noqa: E402  复用 PCANBasic.dll ctypes 封装

# 与固件 can_proto.h 对齐的常量
CAN_OTA_CTRL_ID = 0x200
CAN_OTA_DATA_ID = 0x201
CAN_OTA_REPLY_ID = 0x210
CAN_OTA_ACK_ID = 0x211

CMD_BEGIN = 0x01
CMD_END = 0x02
CMD_STATUS = 0x03
CMD_ABORT = 0x04

REP_BEGIN_OK = 0x81
REP_BEGIN_ERR = 0x82
REP_END_RESULT = 0x83
REP_STATUS = 0x84
REP_STATUS_TOTAL = 0x85
REP_ABORT = 0x86

ACK_OK = 0x81
ACK_ERR = 0x82

OTA_BLOCK = 240  # 与固件 OTA_CHUNK_MAX 一致


def load_ota_secrets():
    """从本地配置读取签名私钥/UID/芯片ID（不入库，避免密钥进 git）。"""
    here = os.path.dirname(os.path.abspath(__file__))
    cfg = os.path.abspath(os.path.join(here, r"..\..\..\HOST\OTA_Tool\local_keys.json"))
    if not os.path.exists(cfg):
        sys.exit("缺少本地密钥配置，请创建 " + cfg +
                 " （字段 private_key / device_uid / chip_id）")
    import json
    with open(cfg, "r", encoding="utf-8") as f:
        d = json.load(f)
    return d["private_key"], d["device_uid"], int(d.get("chip_id", 0x413))


def open_can():
    """打开 PCAN-USB 通道（1Mbps）；失败给出可读错误并退出。"""
    dll = dt._load_pcan_dll()
    if dll is None:
        sys.exit("PCAN-USB 驱动未安装（PCANBasic.dll 缺失）")
    api = dt._PcanApi(dll)
    rc = api.initialize()
    if rc != dt._PCAN_ERROR_OK:
        sys.exit(f"CAN 初始化失败: {api.error_text(rc)}")
    return api


def send(api, mid, payload):
    """发一帧标准帧（≤8B）。"""
    msg = dt._PcanMsg()
    msg.id = mid
    msg.msgtype = dt._PCAN_MESSAGE_STANDARD
    msg.len = len(payload)
    for i, b in enumerate(payload):
        msg.data[i] = b
    rc = api.write(msg)
    if rc != dt._PCAN_ERROR_OK:
        raise RuntimeError(f"CAN 发送失败: {api.error_text(rc)}")


def recv_reply(api, timeout=3.0):
    """等待设备 0x210 应答帧；返回 data 或 None。"""
    end = time.time() + timeout
    while time.time() < end:
        r = api.read_raw()
        if r is None:
            time.sleep(0.002)
            continue
        mid, data = r
        if mid == CAN_OTA_REPLY_ID and data:
            return data
    return None


def cmd_status(api):
    """查询 OTA 状态：state / received / total。"""
    send(api, CAN_OTA_CTRL_ID, bytes([CMD_STATUS]))
    rep = recv_reply(api, 2.0)
    if rep is None or rep[0] != REP_STATUS:
        print("STATUS: 无应答")
        return 1
    state = rep[1]
    rx = struct.unpack_from("<I", rep, 2)[0] if len(rep) >= 6 else 0
    rep2 = recv_reply(api, 1.0)
    total = struct.unpack_from("<I", rep2, 1)[0] if rep2 and rep2[0] == REP_STATUS_TOTAL else 0
    names = {0: "IDLE", 1: "RECEIVING", 2: "DONE"}
    print(f"STATUS: state={names.get(state, state)} {rx}/{total} bytes")
    return 0


def cmd_reset(api):
    send(api, CAN_OTA_CTRL_ID, bytes([CMD_ABORT]))
    rep = recv_reply(api, 2.0)
    print("ABORT:", "OK" if rep and rep[0] == REP_ABORT else "无应答")
    return 0


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    api = open_can()
    try:
        if args[0] == "--status":
            return cmd_status(api)
        if args[0] == "--reset":
            return cmd_reset(api)
        if len(args) < 2:
            print(__doc__)
            return 1
        fw = args[0]
        version = int(args[1])
        build = int(args[2]) if len(args) > 2 else 0
        if not os.path.exists(fw):
            print(f"FAIL: 固件文件不存在 {fw}")
            return 1
        # 加密签名成 OTA 包（BOOT 验签必需；不能裸发 APP.bin）
        sys.path.insert(0, r"D:\GIT-SPACE\D00\HOST\OTA_Tool")
        from core.ymodem_sender import encrypt_and_sign, derive_aes_key_from_uid  # noqa: E402
        priv, uid, chip = load_ota_secrets()
        aes = derive_aes_key_from_uid(uid)
        pkg = os.path.join(os.path.dirname(fw), "_ota_can_pkg.bin")
        encrypt_and_sign(fw, pkg, priv, aes.hex(), version, chip, build)
        with open(pkg, "rb") as f:
            blob = f.read()
        size = len(blob)
        print(f"[OTA-CAN] file={fw} pkg={size}B v{version} b{build}")

        # 1) BEGIN：size LE32 + version LE16 + 保留（8B 单帧）
        send(api, CAN_OTA_CTRL_ID,
             bytes([CMD_BEGIN]) + struct.pack("<IHB", size, version, 0))
        rep = recv_reply(api, 3.0)
        if rep is None or rep[0] != REP_BEGIN_OK:
            print(f"FAIL: BEGIN 被拒绝 code={rep[1] if rep else '无应答'}")
            return 1
        print("BEGIN OK")

        # 2) 数据流：8 块滑动窗口 + 设备 ACK(0x211) 背压（速率拉满）。
        #    基准实测（240B/块 × 35 帧）：
        #      旧：逐帧节流 0.15ms + 单块同步等 ACK → 9.6 KB/s；
        #      新：帧间零延时 + 8 块在飞（窗口背压）→ 53.3 KB/s，
        #      已贴 1Mbps 总线物理极限（~52.7KB/s），全尺寸 237KB 4.3s、
        #      设备端 0 丢帧。窗口再大无增益（总线即瓶颈）。
        #    安全兜底：ACK 停滞 3s → 从最后确认块回卷重传（设备端
        #    Ota_Data 偏移失配时回实际进度 ACK，幂等可恢复）。
        WINDOW_BLOCKS = 8
        sent_bytes = 0
        acked_bytes = 0
        stall_t0 = time.time()
        t0 = time.time()
        last_pct = -1
        while sent_bytes < size:
            if sent_bytes - acked_bytes >= WINDOW_BLOCKS * OTA_BLOCK:
                # 窗口满：等 ACK 腾出在飞额度（0.2ms 轮询，低延迟）
                r = api.read_raw()
                if r is None:
                    if time.time() - stall_t0 > 3.0:
                        # 停滞回卷：从最后确认块重发（含 ACK 丢失对齐）
                        sent_bytes = (acked_bytes // OTA_BLOCK) * OTA_BLOCK
                        print(f"  ... resend block at {sent_bytes}")
                        for frame in dt.encode_can_line(
                                blob[sent_bytes:sent_bytes + OTA_BLOCK]):
                            send(api, CAN_OTA_DATA_ID, frame)
                        stall_t0 = time.time()
                    time.sleep(0.0002)
                    continue
                mid, data = r
                if mid == CAN_OTA_ACK_ID and data and data[0] == ACK_OK:
                    rx = struct.unpack_from("<I", data, 1)[0] if len(data) >= 5 else 0
                    if rx > acked_bytes:
                        acked_bytes = rx
                        stall_t0 = time.time()
                        if acked_bytes > sent_bytes:
                            sent_bytes = acked_bytes   # 设备实际进度对齐
                elif mid == CAN_OTA_ACK_ID and data and data[0] == ACK_ERR:
                    print(f"FAIL: 设备块写入错误 at {acked_bytes}")
                    return 1
                continue
            # 发送下一块：帧间零延时（PCAN 缓冲满时 send() 自动重试不丢帧）
            chunk = blob[sent_bytes:sent_bytes + OTA_BLOCK]
            for frame in dt.encode_can_line(chunk):
                send(api, CAN_OTA_DATA_ID, frame)
            sent_bytes += len(chunk)
            pct = sent_bytes * 100 // size
            if pct >= last_pct + 10:
                last_pct = pct
                print(f"  ... {pct}% ({sent_bytes}/{size})")
        # 收尾：排空在飞 ACK，直到全部确认
        deadline = time.time() + 10.0
        while acked_bytes < size and time.time() < deadline:
            r = api.read_raw()
            if r is None:
                time.sleep(0.0002)
                continue
            mid, data = r
            if mid == CAN_OTA_ACK_ID and data and data[0] == ACK_OK:
                rx = struct.unpack_from("<I", data, 1)[0] if len(data) >= 5 else 0
                if rx > acked_bytes:
                    acked_bytes = rx
        if acked_bytes < size:
            print(f"FAIL: 尾 ACK 未收齐 {acked_bytes}/{size}")
            return 1
        dt_sec = time.time() - t0
        print(f"DATA queued: {size} bytes in {dt_sec:.2f}s "
              f"(host-side {size / dt_sec / 1024:.1f} KB/s)")

        # 4) END：触发 BOOT 校验切换
        send(api, CAN_OTA_CTRL_ID, bytes([CMD_END]))
        rep = recv_reply(api, 10.0)
        if rep is not None and rep[0] == REP_END_RESULT:
            rc = rep[1]
            print(f"END: {'upgrade OK, device rebooting' if rc == 0 else f'failed rc={rc}'}")
            return 0 if rc == 0 else 1
        print("END: no reply (device may already be rebooting)")
        return 0
    finally:
        try:
            api.uninitialize()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
