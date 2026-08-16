# -*- coding: utf-8 -*-
"""watch_mcu.py —— MCU 死机监控（COM5 心跳 + 重启日志捕获）

用法：python workflow/watch_mcu.py [--port COM5] [--interval 2]
功能：
  1. 每 interval 秒通过 COM5 发送空命令（\r\n），判断 MCU 是否响应
  2. 记录时间线：正常 / 无响应(死机起始) / 恢复(IWDG 自动复位重启)
  3. 持续抓取 COM5 输出（启动日志/崩溃记录），死机重启后自动识别启动序列
输出：workflow/logs/watch_mcu_timeline.txt（时间线）+ watch_mcu_log.txt（串口日志）
"""
import argparse
import serial
import time
import os
import sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--interval", type=float, default=1.0)
    args = ap.parse_args()

    logdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
    os.makedirs(logdir, exist_ok=True)
    tl = open(os.path.join(logdir, "watch_mcu_timeline.txt"), "a", encoding="utf-8")
    raw = open(os.path.join(logdir, "watch_mcu_log.txt"), "ab")

    dap_debug = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dap_debug.py")

    def stamp():
        return time.strftime("%Y-%m-%d %H:%M:%S")

    def dap_forensic():
        """死机瞬间 DAP 取证（关键：halt 后保持，不 resume 不清 DBGMCU——
        IWDG 保持冻结，现场永久保留；原 safe 模式 resume 会解冻看门狗致复位）"""
        tl.write(f"[{stamp()}] >>> DAP 取证开始（halt 保持模式）<<<\n")
        tl.flush()
        try:
            import sys
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import dap_debug
            out = os.path.join(logdir, "watch_mcu_forensic.txt")
            # 单会话：halt + 全套寄存器/内存，然后 shutdown（target 保持 halt，
            # DBGMCU=0x7F 冻结 IWDG——现场不丢）
            cmds = [
                "init", "halt",
                "reg pc", "reg sp", "reg lr", "reg xpsr",
                "reg primask", "reg basepri", "reg control",
                "mdw 0xE000ED04 1",   # ICSR（活跃中断/挂起）
                "mdw 0xE000ED28 2",   # CFSR + HFSR（故障寄存器）
                "mdw 0x2000005C 2",   # xTickCount + uxTopReadyPriority
                "mdw 0x20000048 2",   # uwTick + SystemCoreClock
                "mdw 0x20000050 1",   # pxCurrentTCB
                "mdw 0x40023874 1",   # RCC_CSR（复位源）
                "mdw 0x40026010 1",   # DMA1_Stream0 CR（cam）
                "mdw 0x40005014 1",   # UART5 SR
                "shutdown",
            ]
            rc, text = dap_debug.run_ocd(cmds, timeout=30)
            with open(out, "w") as f:
                f.write(text)
            tl.write(f"[{stamp()}] DAP 取证完成 rc={rc} -> {os.path.basename(out)}\n")
            tl.write(f"[{stamp()}] !!! 系统保持 HALT（现场冻结），取证后需手动复位 !!!\n")
            tl.flush()
            # 解析 PC/SP（供后续栈回溯）
            import re
            m_pc = re.search(r"pc\s*\(/32\):\s*(0x[0-9a-fA-F]+)", text)
            m_sp = re.search(r"sp\s*\(/32\):\s*(0x[0-9a-fA-F]+)", text)
            if m_pc:
                tl.write(f"[{stamp()}] PC={m_pc.group(1)} SP={m_sp.group(1) if m_sp else '?'}\n")
                tl.flush()
        except Exception as e:
            tl.write(f"[{stamp()}] DAP forensic error: {e}\n")
            tl.flush()

    alive = None          # None=未知 True=活 False=死
    dead_since = 0
    boot_since = 0
    pending = b""         # 串口残余
    dap_done = False

    ser = None
    try:
        ser = serial.Serial(args.port, 115200, timeout=0.3)
    except Exception as e:
        print(f"打开 {args.port} 失败: {e}")
        sys.exit(1)

    tl.write(f"=== watch start {stamp()} (interval={args.interval}s) ===\n")
    tl.flush()

    while True:
        # 1) 收串口数据（日志）
        try:
            n = ser.in_waiting
            if n > 0:
                data = ser.read(n)
                raw.write(data)
                raw.flush()
                pending += data
                # 检测启动序列（死机重启后）
                if b"Boot complete" in pending or b"Module registry" in pending:
                    now = time.time()
                    if boot_since and now - boot_since > 5:
                        tl.write(f"[{stamp()}] *** BOOT DETECTED（死机后重启）***\n")
                        tl.flush()
                        boot_since = 0
                if b"[CRASH]" in pending:
                    tl.write(f"[{stamp()}] *** CRASH REPORT in log ***\n")
                    tl.flush()
                if len(pending) > 4096:
                    pending = pending[-4096:]
        except Exception:
            pass

        # 2) 心跳探测：发 \r\n（空命令，无害）
        try:
            ser.reset_input_buffer()
            ser.write(b"\r\n")
            time.sleep(0.3)
            resp = ser.in_waiting
            if resp > 0:
                ser.read(resp)
                if alive is False:
                    tl.write(f"[{stamp()}] RECOVERED（恢复响应，死机持续 {int(time.time()-dead_since)}s）\n")
                    tl.flush()
                    alive = True
                    dap_done = False
                elif alive is None:
                    tl.write(f"[{stamp()}] MCU ALIVE\n")
                    tl.flush()
                    alive = True
            else:
                if alive is not False:
                    dead_since = time.time()
                    tl.write(f"[{stamp()}] *** DEAD（无响应）— 立即 DAP 取证 ***\n")
                    tl.flush()
                    alive = False
                    # 死机瞬间取证（抢 IWDG ~4s 窗口）
                    if not dap_done:
                        dap_done = True
                        dap_forensic()
                elif time.time() - dead_since > 30 and not boot_since:
                    boot_since = time.time()
        except Exception as e:
            tl.write(f"[{stamp()}] 串口错误: {e}\n")
            tl.flush()
            time.sleep(3)
            try:
                ser.close()
                ser = serial.Serial(args.port, 115200, timeout=0.3)
            except Exception:
                pass

        time.sleep(args.interval)

if __name__ == "__main__":
    main()
