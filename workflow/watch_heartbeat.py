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
        # 取证（OpenOCD 0.12 有效命令：reg <name>；'regs'/'disassemble' 无效！
        # 无效命令会导致会话中止——run_ocd_safe 已拆双会话保证恢复）
        rc, out = dap_debug.run_ocd_safe([
            "init", "halt",
            "reg pc", "reg sp", "reg lr",                # PC/SP/LR
            "mdw 0xE000ED28 1",                          # CFSR
            "mdw 0xE000ED2C 1",                          # HFSR
            "mdw 0xE000ED34 1",                          # MMFAR
            "mdw 0xE000ED38 1",                          # BFAR
            "mdw 0x4002104C 1",                          # RCC_CSR（复位原因）
            "mdw 0xE000ED04 1",                          # ICSR（挂起中断）
        ], timeout=45)
        if rc != 0:
            return f"[dap] session failed rc={rc}"
        for ln in out.splitlines():
            if any(k in ln for k in ("pc ", "sp ", "lr ", "0xe000ed",
                                     "0x4002104c", "0x4002104C")):
                lines.append(ln.strip())
        pc = dap_debug.parse_reg(out, "pc")
        if pc is not None:
            try:
                sym = dap_debug.sym_at(pc)
                if sym:
                    lines.append(f"PC symbol: {sym}")
            except Exception:
                pass
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
                    tl.write(f"[{stamp()}] *** DEAD（连续 {no_resp} 次无响应）*** 进入 60s 观察期\n")
                    tl.flush()
                    # 观察期：OTA 升级/看门狗复位窗口（20-80s）内 shell 无响应是
                    # 正常现象——严禁立即 DAP 取证（halt 会打断 BOOT Flash 擦写，
                    # 实测教训：OTA 升级被打断）。观察期内恢复则记录 RECOVERED；
                    # 持续无响应（真实死机）才触发 DAP 取证。
                    # 25s 时做一次快速 PC 采样：PC 在 APP 区 = 真实死机现场
                    # （运行中 hang 的 PC 就抓这一下——等 60s 可能已进入
                    # 死机-复位循环，PC 落在启动早期失去现场）；
                    # PC 在 BOOT 区（0x0800xxxx）= OTA/复位窗口，放弃。
                    time.sleep(25)
                    try:
                        ser.reset_input_buffer()
                        ser.write(b"\r\n")
                        time.sleep(0.6)
                        if ser.in_waiting > 0:
                            ser.read(ser.in_waiting)
                            tl.write(f"[{stamp()}] RECOVERED（25s 快速采样前已恢复，"
                                     f"判定为 OTA/复位窗口）\n")
                            tl.flush()
                            no_resp = 0
                        else:
                            tl.write(f"[{stamp()}] 25s 仍无响应 → 快速 PC 采样\n")
                            tl.flush()
                            if HAVE_DAP:
                                rc, out = dap_debug.run_ocd_safe(
                                    ["init", "halt", "reg pc", "reg sp",
                                     "reg lr", "mdw 0xE000ED04 1"], timeout=45)
                                pc = dap_debug.parse_reg(out, "pc") if rc == 0 else None
                                if pc is not None and 0x08010000 <= pc < 0x080E0000:
                                    fg.write(f"[{stamp()}] EARLY-SAMPLE(APP区=真死机现场)"
                                             f" pc=0x{pc:08X} "
                                             f"sp=0x{(dap_debug.parse_reg(out,'sp') or 0):08X} "
                                             f"lr=0x{(dap_debug.parse_reg(out,'lr') or 0):08X}\n")
                                    fg.flush()
                                elif pc is not None:
                                    fg.write(f"[{stamp()}] EARLY-SAMPLE(非APP区=OTA/复位窗口)"
                                             f" pc=0x{pc:08X}\n")
                                    fg.flush()
                    except Exception:
                        pass
                    # 观察期剩余时间继续探测
                    dead_t0 = time.time()
                    while time.time() - dead_t0 < 35.0:
                        time.sleep(2)
                        try:
                            ser.reset_input_buffer()
                            ser.write(b"\r\n")
                            time.sleep(0.6)
                            if ser.in_waiting > 0:
                                ser.read(ser.in_waiting)
                                tl.write(f"[{stamp()}] RECOVERED（观察期内恢复，"
                                         f"判定为 OTA/复位窗口，非死机）\n")
                                tl.flush()
                                no_resp = 0
                                break
                        except Exception:
                            pass
                    if no_resp != 0:
                        tl.write(f"[{stamp()}] *** 观察期 60s 后仍无响应 = 真实死机，"
                                 f"触发 DAP 取证 ***\n")
                        tl.flush()
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
