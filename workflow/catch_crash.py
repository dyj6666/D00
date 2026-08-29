#!/usr/bin/env python
"""catch_crash —— 启动崩溃捕获（DAP 断点 + 全量故障取证）

用法：python workflow/catch_crash.py [--com COM5] [--wait-s 180] [--bp-addr 0x080101B9]

流程：
  1. 打开 APP shell 串口，阻塞等待启动横幅（APP 已从 BOOT 复位进入运行）；
  2. 立即 OpenOCD 会话：halt -> 冻结 IWDG/WWDG(DBGMCU_CR) -> 在 HardFault_Handler
     入口布硬件断点 -> resume -> 等待（默认 12s）；
  3. 若断点命中：读取 CFSR/HFSR/BFAR/MMFAR、MSP/PSP、异常帧（r0..xpsr 8 字）、
     当前寄存器，输出完整取证；
  4. 无论是否命中：清断点、清 DBGMCU 冻结、resume 恢复固件运行（独立恢复会话）；
  5. 再读串口数秒，抓崩溃恢复摘要与后续启动日志。

用途：定位"启动后 ~1.7s 周期性 HardFault"这类崩得快、复位快的故障。
发布构建 IWDG 开启，halt 期间必须冻结看门狗，否则 1s 即复位丢失现场。
"""
import argparse
import re
import sys
import time

import serial

from dap_debug import DBGMCU_CR, run_ocd, run_ocd_safe

BANNER_PATTERNS = [b"D00 Embedded Platform", b"Firmware v", b"Module registry"]
HARDFAULT_ENTRY = 0x080101B9  # HardFault_Handler（可从 axf 符号表确认）
CFSR = 0xE000ED28
HFSR = 0xE000ED2C
MMFAR = 0xE000ED34
BFAR = 0xE000ED38
AFSR = 0xE000ED3C


def wait_banner(port, timeout_s):
    """等待 APP 启动横幅；返回抓到的首行文本或 None。"""
    t0 = time.time()
    line = b""
    ser = serial.Serial(port, 115200, timeout=0.2)
    try:
        while time.time() - t0 < timeout_s:
            d = ser.read(256)
            if not d:
                continue
            line += d
            # 逐行检查（保留最后 256B 窗口即可匹配跨块横幅）
            for pat in BANNER_PATTERNS:
                if pat in line:
                    print(f"[catch] banner hit: {pat.decode()}")
                    return ser
            if len(line) > 4096:
                line = line[-512:]
    finally:
        # 调用方负责关闭
        pass
    print("[catch] timeout: no APP banner on " + port)
    try:
        ser.close()
    except Exception:
        pass
    return None


def decode_fault(out):
    """从 OpenOCD 输出解析故障寄存器与异常帧。"""
    def get(pattern):
        m = re.search(pattern, out)
        return m.group(1) if m else None

    pc = get(r"pc\s*=\s*(0x[0-9a-fA-F]+)")
    print(f"\n=== FAULT DECODE ===")
    print(f"halted PC     : {pc}")
    for name, addr in (("CFSR", CFSR), ("HFSR", HFSR),
                       ("MMFAR", MMFAR), ("BFAR", BFAR), ("AFSR", AFSR)):
        m = re.search(rf"{addr:X}:\s*(0x[0-9a-fA-F]+)", out)
        print(f"{name:6s} @{addr:08X}: {m.group(1) if m else 'N/A'}")
    # 异常帧（MSP 处 8 字：r0 r1 r2 r3 r12 lr pc xpsr）
    m = re.search(r"msp\s*=\s*(0x[0-9a-fA-F]+)", out)
    if m:
        print(f"MSP: {m.group(1)}  (异常帧: r0 r1 r2 r3 r12 lr pc xpsr)")
    m = re.search(r"psp\s*=\s*(0x[0-9a-fA-F]+)", out)
    if m:
        print(f"PSP: {m.group(1)}")
    # 打印原始寄存器与内存转储（完整保留）
    print("\n--- raw session tail ---")
    print(out[-3000:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--com", default="COM5")
    ap.add_argument("--wait-s", type=int, default=180, help="等横幅超时")
    ap.add_argument("--bp-wait-ms", type=int, default=12000, help="断点等待")
    ap.add_argument("--bp-addr", default=None, help="断点地址（默认 HardFault_Handler）")
    args = ap.parse_args()

    bp_addr = int(args.bp_addr, 0) if args.bp_addr else HARDFAULT_ENTRY

    ser = wait_banner(args.com, args.wait_s)
    if ser is None:
        sys.exit(1)

    # ---- 取证会话：halt + 冻结看门狗 + 断点 + 等待 ----
    cmds = [
        "init", "halt",
        f"mww 0x{DBGMCU_CR:X} 0x1FF",          # 冻结 IWDG/WWDG + 调试模式
        f"bp 0x{bp_addr:x} 2 hw",
        "resume",
        f"sleep {args.bp_wait_ms}",
        "halt",
        "reg pc",
        "reg sp",
        "reg lr",
        "reg msp",
        "reg psp",
        f"mdw 0x{CFSR:X}",
        f"mdw 0x{HFSR:X}",
        f"mdw 0x{MMFAR:X}",
        f"mdw 0x{BFAR:X}",
        f"mdw 0x{AFSR:X}",
        "rbp all",
        "resume",
        "shutdown",
    ]
    print(f"[catch] DAP session: bp @ 0x{bp_addr:X}, wait {args.bp_wait_ms} ms ...")
    rc, out = run_ocd(cmds, timeout=max(30, args.bp_wait_ms // 1000 + 20))

    # ---- 独立恢复会话（双会话保险） ----
    for attempt in range(2):
        r2, _ = run_ocd(["init", f"mww 0x{DBGMCU_CR:X} 0x00000000",
                         "resume", "shutdown"], timeout=15)
        if r2 == 0:
            break
    print(f"[catch] forensic rc={rc}")

    pc = re.search(r"pc\s*=\s*(0x[0-9a-fA-F]+)", out)
    hit = pc is not None and int(pc.group(1), 16) == bp_addr
    if hit:
        print("[catch] HIT: HardFault_Handler 入口断点命中 —— 捕获到崩溃现场!")
        decode_fault(out)
    else:
        print(f"[catch] 断点未命中（{args.bp_wait_ms}ms 内无崩溃）："
              f"pc={pc.group(1) if pc else 'N/A'}")

    # ---- 崩溃后串口观察（复位摘要 / 启动循环） ----
    print("[catch] post-mortem serial watch 8s ...")
    ser.timeout = 0.3
    t0 = time.time()
    buf = b""
    while time.time() - t0 < 8:
        d = ser.read(512)
        if d:
            buf += d
            if len(buf) > 2048:
                buf = buf[-1024:]
    ser.close()
    text = buf.decode("utf-8", "replace")
    for kw in ("CRASH", "Previous crash", "HardFault", "WAV", "AUD", "Boot complete"):
        if kw in text:
            print(f"[catch] post: {kw}: ...")
    print("--- post-mortem tail ---")
    print(text[-1500:])
    print("--- end ---")


if __name__ == "__main__":
    main()
