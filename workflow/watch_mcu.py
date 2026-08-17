# -*- coding: utf-8 -*-
"""watch_mcu.py —— MCU 死机监控 v4（DAP 直读 uwTick 精确检测版）

用法：python workflow/watch_mcu.py [--port COM5] [--interval 2.0]
检测：OpenOCD 不 halt 直读 uwTick（每 interval 秒，零干扰）——
      uwTick 连续 2 次不变 → 死机确认（tick 停止 = 中断系统瘫痪）
辅助：COM5 抓取启动日志 / [CRASH] 报告
取证：halt 保持全套（寄存器/中断全貌/时钟/BKP/双栈）→ 时间戳存档
      （16.4s IWDG 窗口内完成；调试构建下 ERR/WDOG 软复位已关）
"""
import argparse
import serial
import time
import os
import sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--interval", type=float, default=2.0)
    args = ap.parse_args()

    logdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
    os.makedirs(logdir, exist_ok=True)
    tl = open(os.path.join(logdir, "watch_mcu_timeline.txt"), "a", encoding="utf-8")
    raw = open(os.path.join(logdir, "watch_mcu_log.txt"), "ab")

    def stamp():
        return time.strftime("%Y-%m-%d %H:%M:%S")

    def ts_name():
        return time.strftime("forensic_%Y%m%d_%H%M%S.txt")

    def dap_read_tick():
        """不 halt 直读 uwTick（运行态零干扰）；失败返回 None（不误判死机）"""
        try:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import dap_debug
            rc, out = dap_debug.run_ocd(["init", "mdw 0x20000048 1", "shutdown"], 15)
            if rc != 0:
                return None
            for line in out.splitlines():
                if "0x20000048" in line:
                    m = __import__("re").search(r":\s*([0-9a-fA-F]{8})", line)
                    if m:
                        return int(m.group(1), 16)
            return None
        except Exception:
            return None

    def dap_forensic():
        """死机瞬间 DAP 取证（halt 保持：不 resume 不清 DBGMCU，IWDG 冻结）。"""
        tl.write(f"[{stamp()}] >>> DAP 取证开始（halt 保持模式）<<<\n")
        tl.flush()
        try:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import dap_debug
            out = os.path.join(logdir, ts_name())
            cmds = [
                "init", "halt",
                "reg pc", "reg sp", "reg lr", "reg xpsr",
                "reg primask", "reg basepri", "reg control", "reg faultmask",
                "reg msp", "reg psp",
                "mdw 0xE000ED04 1",   # ICSR
                "mdw 0xE000ED28 4",   # CFSR + HFSR + MMFAR + BFAR
                "mdw 0xE000ED20 2",   # SHPR2 + SHPR3
                "mdw 0xE000E100 3",   # NVIC ISER0-2
                "mdw 0xE000E200 3",   # NVIC ISPR0-2
                "mdw 0xE000ED0C 1",   # AIRCR
                "mdw 0xE0042004 1",   # DBGMCU_CR
                "mdw 0x40023800 4",   # RCC_CR/PLLCFGR/CFGR/CIR
                "mdw 0x40023874 1",   # RCC_CSR
                "mdw 0x40001400 8",   # TIM7 CR1..CCMR2
                "mdw 0x40001420 4",   # TIM7 CCER/CNT/PSC/ARR
                "mdw 0xE000E010 3",   # SYST_CSR/RVR/CVR
                "mdw 0x40005000 4",   # UART5 SR/DR/BRR/CR1
                "mdw 0x40026010 6",   # DMA1_Stream0 CR/NDTR/PAR/M0AR/M1AR/FCR
                "mdw 0x40026034 2",   # DMA1_Stream3（USART3 TX）CR/NDTR
                "mdw 0x20000048 2",   # uwTick + uwTickFreq
                "mdw 0x2000005C 2",   # xTickCount + uxCurrentNumberOfTasks
                "mdw 0x20000050 1",   # pxCurrentTCB
                "mdw 0x20000080 1",   # uxSchedulerSuspended
                "mdw 0x20000064 1",   # xSchedulerRunning
                "mdw 0x40002850 16",  # RTC BKP（err_mgr 崩溃摘要）
                "shutdown",
            ]
            rc, text = dap_debug.run_ocd(cmds, timeout=60)
            # PSP/MSP 栈读取（第二/三次会话）
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

    alive = None
    last_tick = None
    stall_count = 0
    pending = b""
    dap_done = False

    ser = None
    try:
        ser = serial.Serial(args.port, 115200, timeout=0.3)
    except Exception as e:
        print(f"打开 {args.port} 失败: {e}")
        sys.exit(1)

    tl.write(f"=== watch start {stamp()} (interval={args.interval}s, v4 DAP-tick 检测) ===\n")
    tl.flush()

    while True:
        # 1) COM5 收串口数据（日志/启动序列/CRASH）
        try:
            n = ser.in_waiting
            if n > 0:
                data = ser.read(n)
                raw.write(data)
                raw.flush()
                pending += data
                if b"Boot complete" in pending:
                    tl.write(f"[{stamp()}] *** BOOT DETECTED（重启）***\n")
                    tl.flush()
                    pending = b""   # 清空防重复检测（"Boot complete" 残留会每轮误报）
                if b"[CRASH]" in pending:
                    tl.write(f"[{stamp()}] *** CRASH REPORT in log ***\n")
                    tl.flush()
                    pending = b""
                if len(pending) > 4096:
                    pending = pending[-4096:]
        except Exception:
            pass

        # 2) DAP 直读 uwTick（主检测——零干扰）
        tick = dap_read_tick()
        if tick is not None:
            if last_tick is not None and tick == last_tick:
                stall_count += 1
            else:
                stall_count = 0
                # RECOVERED 判定：uwTick 必须超过阈值（系统真正运行）——
                # 避免"取证后 resume 造成 86→88 假递增"误判恢复（无限取证循环）
                if tick > 1000:
                    if alive is False:
                        tl.write(f"[{stamp()}] RECOVERED（uwTick={tick} 真正运行，死机持续 {int(time.time()-dead_since)}s）\n")
                        tl.flush()
                        alive = True
                        dap_done = False
                    elif alive is None:
                        tl.write(f"[{stamp()}] MCU ALIVE (uwTick={tick})\n")
                        tl.flush()
                        alive = True
                elif alive is None:
                    tl.write(f"[{stamp()}] MCU ALIVE (uwTick={tick})\n")
                    tl.flush()
                    alive = True
            last_tick = tick
            if stall_count >= 2 and alive is not False:
                dead_since = time.time()
                tl.write(f"[{stamp()}] *** DEAD（uwTick 连续 {stall_count} 次不变 = {tick}）— 立即 DAP 取证 ***\n")
                tl.flush()
                alive = False
                if not dap_done:
                    dap_done = True
                    dap_forensic()
        else:
            tl.write(f"[{stamp()}] DAP 读失败（跳过本轮，不误判）\n")
            tl.flush()

        time.sleep(args.interval)

if __name__ == "__main__":
    main()
