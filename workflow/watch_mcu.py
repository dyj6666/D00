# -*- coding: utf-8 -*-
"""watch_mcu.py —— MCU 死机监控 v3（调试构建取证版）

用法：python workflow/watch_mcu.py [--port COM5] [--interval 1.0]
功能：
  1. 每 interval 秒通过 COM5 发送空命令（\r\n），判断 MCU 是否响应
  2. 死机检测（连续 3 次无响应）后立即 DAP 取证（halt 保持模式）：
     - 全套寄存器（PC/SP/LR/XPSR/屏蔽位/MSP/PSP）
     - 故障寄存器（ICSR/CFSR/HFSR/MMFAR/BFAR/优先级）
     - 中断全貌（NVIC ISER0-2/ISPR0-2）
     - 时钟（RCC CR/PLLCFGR/CFGR/CSR）+ TIM7 + SysTick
     - UART5 + DMA1_Stream0（cam_link 链路）
     - FreeRTOS（xTickCount/uwTick/pxCurrentTCB/TCB/任务名）
     - RTC BKP（err_mgr 崩溃摘要——复位后保留）
     - PSP + MSP 栈内容（64 字各，供回溯）
  3. 取证输出：logs/forensic_<时间戳>.txt（不覆盖）+ 时间线记录
  4. 调试构建（APP_DEBUG_MODE=1 关 IWDG/WDOG/ERR 复位）下死机后
     系统不复位——现场永久保留，随时可补取证
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

    def stamp():
        return time.strftime("%Y-%m-%d %H:%M:%S")

    def ts_name():
        return time.strftime("forensic_%Y%m%d_%H%M%S.txt")

    def dap_forensic():
        """死机瞬间 DAP 取证（halt 保持：不 resume 不清 DBGMCU，IWDG 冻结，
        现场永久保留；调试构建下 IWDG 本已关闭，双保险）。"""
        tl.write(f"[{stamp()}] >>> DAP 取证开始（halt 保持模式）<<<\n")
        tl.flush()
        try:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import dap_debug
            out = os.path.join(logdir, ts_name())
            cmds = [
                "init", "halt",
                # ---- 核心寄存器 ----
                "reg pc", "reg sp", "reg lr", "reg xpsr",
                "reg primask", "reg basepri", "reg control", "reg faultmask",
                "reg msp", "reg psp",
                # ---- 故障/中断状态 ----
                "mdw 0xE000ED04 1",   # ICSR
                "mdw 0xE000ED28 4",   # CFSR + HFSR + MMFAR + BFAR
                "mdw 0xE000ED20 2",   # SHPR2 + SHPR3（SVC/PendSV/SysTick 优先级）
                "mdw 0xE000E100 3",   # NVIC ISER0-2（中断使能全貌）
                "mdw 0xE000E200 3",   # NVIC ISPR0-2（中断挂起全貌）
                "mdw 0xE000ED0C 1",   # AIRCR（优先级分组）
                "mdw 0xE0042004 1",   # DBGMCU_CR（冻结残留）
                # ---- 时钟 ----
                "mdw 0x40023800 4",   # RCC_CR/PLLCFGR/CFGR/CIR
                "mdw 0x40023874 1",   # RCC_CSR（复位源）
                # ---- 定时器（HAL timebase = TIM7）+ SysTick ----
                "mdw 0x40001400 8",   # TIM7 CR1..CCMR2
                "mdw 0x40001420 4",   # TIM7 CCER/CNT/PSC/ARR
                "mdw 0xE000E010 3",   # SYST_CSR/RVR/CVR
                # ---- UART5（cam_link）+ DMA1_Stream0 ----
                "mdw 0x40005000 4",   # UART5 SR/DR/BRR/CR1
                "mdw 0x40026010 6",   # DMA1_Stream0 CR/NDTR/PAR/M0AR/M1AR/FCR
                # ---- FreeRTOS 核心 ----
                "mdw 0x20000048 2",   # uwTick + uwTickFreq
                "mdw 0x2000005C 2",   # xTickCount + uxCurrentNumberOfTasks
                "mdw 0x20000050 1",   # pxCurrentTCB
                "mdw 0x20000080 1",   # uxSchedulerSuspended
                "mdw 0x20000064 1",   # xSchedulerRunning
                # ---- RTC BKP（err_mgr 崩溃摘要，复位后保留）----
                "mdw 0x40002850 16",  # BKP0R-BKP15R
                # ---- 栈内容（回溯用）----
                "shutdown",
            ]
            rc, text = dap_debug.run_ocd(cmds, timeout=60)
            # 追加 PSP 栈读取（需要 SP 值——第二次会话）
            sp = None
            for line in text.splitlines():
                m = __import__("re").search(r"sp\s*\(/32\):\s*(0x[0-9a-fA-F]+)", line)
                if m:
                    sp = int(m.group(1), 16)
                    break
            if sp:
                rc2, text2 = dap_debug.run_ocd(
                    ["init", "halt", f"mdw 0x{sp:X} 64", "shutdown"], timeout=30)
                text += "\n==== PSP stack (64 words) ====\n" + text2
            msp = None
            for line in text.splitlines():
                m = __import__("re").search(r"msp\s*\(/32\):\s*(0x[0-9a-fA-F]+)", line)
                if m:
                    msp = int(m.group(1), 16)
                    break
            if msp:
                rc3, text3 = dap_debug.run_ocd(
                    ["init", "halt", f"mdw 0x{msp:X} 64", "shutdown"], timeout=30)
                text += "\n==== MSP stack (64 words) ====\n" + text3

            with open(out, "w") as f:
                f.write(text)
            tl.write(f"[{stamp()}] DAP 取证完成 rc={rc} -> {os.path.basename(out)}\n")
            tl.write(f"[{stamp()}] !!! 系统保持 HALT（现场冻结），取证后需手动复位 !!!\n")
            tl.flush()
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
    no_resp_count = 0
    pending = b""
    dap_done = False

    ser = None
    try:
        ser = serial.Serial(args.port, 115200, timeout=0.3)
    except Exception as e:
        print(f"打开 {args.port} 失败: {e}")
        sys.exit(1)

    tl.write(f"=== watch start {stamp()} (interval={args.interval}s, 调试构建取证版) ===\n")
    tl.flush()

    while True:
        # 1) 收串口数据（日志/启动序列）
        try:
            n = ser.in_waiting
            if n > 0:
                data = ser.read(n)
                raw.write(data)
                raw.flush()
                pending += data
                if b"Boot complete" in pending or b"Module registry" in pending:
                    now = time.time()
                    if alive is not True:
                        tl.write(f"[{stamp()}] *** BOOT DETECTED（死机后重启）***\n")
                        tl.flush()
                if b"[CRASH]" in pending:
                    tl.write(f"[{stamp()}] *** CRASH REPORT in log ***\n")
                    tl.flush()
                    pending = b""   # 清除已检测内容，防止重复报警
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
                data = ser.read(resp)
                raw.write(data)
                raw.flush()
                no_resp_count = 0
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
                no_resp_count += 1
                if no_resp_count >= 3 and alive is not False:
                    dead_since = time.time()
                    tl.write(f"[{stamp()}] *** DEAD（连续 {no_resp_count} 次无响应）— 立即 DAP 取证 ***\n")
                    tl.flush()
                    alive = False
                    if not dap_done:
                        dap_done = True
                        dap_forensic()
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
