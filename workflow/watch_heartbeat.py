# -*- coding: utf-8 -*-
"""watch_heartbeat.py v2 —— COM5 心跳监控 + 死机 DAP 自动取证

每 2s 发 \r\n 探测 shell 响应；连续 3 次无响应 = 死机：
  1. 立即 OpenOCD halt 取证（PC/SP/LR/异常寄存器/复位原因/PC 反汇编/异常栈帧）
  2. resume 后继续监控（发布固件 IWDG 4.1s 会自动复位，观察 DEAD→RECOVERED 周期）
输出：workflow/logs/heartbeat_timeline.txt（时间线）+ heartbeat_forensics.txt（取证）
"""
import serial
import time
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "workflow"))

try:
    import dap_debug
    HAVE_DAP = True
except Exception:
    HAVE_DAP = False


def stamp():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def dap_forensics(tl):
    """死机现场取证：halt 读 PC/异常寄存器/复位原因/栈帧。返回描述文本。"""
    if not HAVE_DAP:
        return "[dap] dap_debug unavailable, skip forensics"
    lines = []
    try:
        # halt + 冻结外设 → 取证 → resume（run_ocd_safe 自动恢复 DBGMCU）
        rc, out = dap_debug.run_ocd_safe([
            "halt",
            "regs",                                   # PC/SP/LR/R0-12/xPSR
            "mdw 0xE000ED28 1",                       # CFSR
            "mdw 0xE000ED2C 1",                       # HFSR
            "mdw 0xE000ED34 1",                       # MMFAR
            "mdw 0xE000ED38 1",                       # BFAR
            "mdw 0x4002104C 1",                       # RCC_CSR（复位原因）
            "disassemble pc 4",                       # PC 附近 4 条指令
        ], timeout=45)
        if rc != 0:
            return f"[dap] session failed rc={rc}"
        for ln in out.splitlines():
            if any(k in ln for k in ("pc", "sp", "lr", "r0", "r1", "r2", "r3",
                                     "xpsr", "0xe000ed", "0x4002104c",
                                     "0x4002104C", "0x:", "=>")):
                lines.append(ln.strip())
        pc = dap_debug.parse_reg(out, "pc")
        if pc is not None:
            sym = dap_debug.sym_at(pc) if hasattr(dap_debug, "sym_at") else None
            if sym:
                lines.append(f"PC symbol: {sym}")
        return "[dap] FORENSICS:\n" + "\n".join(lines[:40])
    except Exception as e:
        return f"[dap] forensics error: {e}"


def main():
    logdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
    os.makedirs(logdir, exist_ok=True)
    tl = open(os.path.join(logdir, "heartbeat_timeline.txt"), "a", encoding="utf-8")
    fg = open(os.path.join(logdir, "heartbeat_forensics.txt"), "a", encoding="utf-8")

    ser = serial.Serial("COM5", 115200, timeout=0.5)
    ser.reset_input_buffer()
    tl.write(f"=== heartbeat watch v2 start {stamp()} ===\n")
    tl.flush()

    no_resp = 0
    while True:
        try:
            ser.reset_input_buffer()
            ser.write(b"\r\n")
            time.sleep(0.6)
            n = ser.in_waiting
            if n > 0:
                ser.read(n)
                if no_resp >= 3:
                    tl.write(f"[{stamp()}] RECOVERED（死机持续约 {no_resp*2}s 后恢复）\n")
                    tl.flush()
                no_resp = 0
            else:
                no_resp += 1
                if no_resp == 3:
                    tl.write(f"[{stamp()}] *** DEAD（连续 {no_resp} 次无响应 = 死机）***\n")
                    tl.flush()
                    # 死机取证（独立线程，避免阻塞心跳循环太久）
                    f = dap_forensics(tl)
                    fg.write(f"[{stamp()}] DEAD\n{f}\n")
                    fg.flush()
        except Exception as e:
            tl.write(f"[{stamp()}] 串口错误: {e}\n")
            tl.flush()
            time.sleep(5)
            try:
                ser.close()
                ser = serial.Serial("COM5", 115200, timeout=0.5)
            except Exception:
                pass
        time.sleep(2)


if __name__ == "__main__":
    main()
