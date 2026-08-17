# -*- coding: utf-8 -*-
"""analyze_forensic.py —— 死机取证文件自动分析（符号化）

用法：python workflow/analyze_forensic.py [forensic文件]
     不传参数则分析 logs/ 下最新的 forensic_*.txt

输出：logs/analyze_<同名>.txt（符号化报告）
"""
import os
import re
import sys
import glob
import time

MAP = r"D:\GIT-SPACE\D00\APP\APP\MDK-ARM\APP\APP.map"

# F407 IRQn -> 外设名（常用）
IRQ_TABLE = {
    0: "WWDG", 1: "PVD", 2: "TAMP_STAMP", 3: "RTC_WKUP", 4: "FLASH", 5: "RCC",
    6: "EXTI0", 7: "EXTI1", 8: "EXTI2", 9: "EXTI3", 10: "EXTI4",
    11: "DMA1_Stream0", 12: "DMA1_Stream1", 13: "DMA1_Stream2", 14: "DMA1_Stream3",
    15: "DMA1_Stream4", 16: "DMA1_Stream5", 17: "DMA1_Stream6", 18: "DMA1_Stream7",
    19: "ADC1_2", 20: "USB_HP_CAN1_TX", 21: "USB_LP_CAN1_RX0", 22: "CAN1_RX1",
    23: "CAN1_SCE", 24: "EXTI9_5", 25: "TIM1_BRK_TIM9", 26: "TIM1_UP_TIM10",
    27: "TIM1_TRG_COM_TIM11", 28: "TIM1_CC", 29: "TIM2", 30: "TIM3", 31: "TIM4",
    32: "I2C1_EV", 33: "I2C1_ER", 34: "I2C2_EV", 35: "I2C2_ER", 36: "SPI1",
    37: "SPI2", 38: "USART1", 39: "USART2", 40: "USART3", 41: "EXTI15_10",
    42: "RTC_Alarm", 43: "OTG_FS_WKUP", 44: "TIM8_BRK_TIM12", 45: "TIM8_UP_TIM13",
    46: "TIM8_TRG_COM_TIM14", 47: "TIM8_CC", 48: "DMA1_Stream4?", 49: "SDIO",
    50: "TIM5", 51: "SPI3", 52: "UART4", 53: "UART5", 54: "TIM6_DAC",
    55: "TIM7", 56: "DMA2_Stream0", 57: "DMA2_Stream1", 58: "DMA2_Stream2",
    59: "DMA2_Stream3", 60: "DMA2_Stream4", 61: "ETH", 62: "ETH_WKUP",
    63: "CAN2_TX", 64: "CAN2_RX0", 65: "CAN2_RX1", 66: "CAN2_SCE", 67: "OTG_FS",
    68: "DMA2_Stream5", 69: "DMA2_Stream6", 70: "DMA2_Stream7", 71: "USART6",
    72: "I2C3_EV", 73: "I2C3_ER", 77: "OTG_HS", 78: "DCMI", 79: "CRYP",
    80: "HASH_RNG", 81: "FPU",
}

# err_mgr BKP 布局（reg0 保留 OTA）
BKP = {
    "MAGIC": 1, "SRC": 2, "SEQ": 3, "PC": 4, "LR": 5, "ADDR": 6,
    "CFSR": 7, "HFSR": 8, "TICK": 9, "NAME0": 10, "NAME1": 11,
    "NAME2": 12, "CRC": 13, "RAPID": 14, "RTC": 15,
}
ERR_SRC = ["NMI", "HardFault", "MemManage", "BusFault", "UsageFault",
           "RTOS Assert", "Stack Overflow", "Task Stall", "Unhandled IRQ"]


def load_symbols(map_path):
    """解析 Keil map 全局符号表：地址(清 Thumb 位) -> 函数名"""
    syms = {}
    try:
        with open(map_path, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except Exception as e:
        print("map 读取失败:", e)
        return syms
    in_global = False
    for ln in lines:
        if "Global Symbols" in ln:
            in_global = True
            continue
        if not in_global:
            continue
        m = re.match(r"\s+(\S+)\s+(0x[0-9A-Fa-f]+)\s+Thumb\s+Code", ln)
        if m:
            addr = int(m.group(2), 16) & ~1
            syms[addr] = m.group(1)
    return syms


def lookup(syms, addr):
    """地址 -> 包含它的函数名（最近低地址符号）"""
    addr &= ~1
    best = None
    for a in syms:
        if a <= addr and (best is None or a > best):
            best = a
    if best is None:
        return "???"
    return f"{syms[best]}+0x{addr-best:X}"


def parse_forensic(path):
    """解析取证文本：地址 -> 值"""
    regs = {}
    mem = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            m = re.match(r"(\w+)\s*\(/(?:32|1|3|8)\):\s*(0x[0-9a-fA-F]+)", ln)
            if m:
                regs[m.group(1)] = int(m.group(2), 16)
                continue
            m = re.match(r"(0x[0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{8}\s*)+)", ln)
            if m:
                base = int(m.group(1), 16)
                words = [int(x, 16) for x in m.group(2).split()]
                for i, w in enumerate(words):
                    mem[base + i * 4] = w
    return regs, mem


def decode_cfsr(cfsr):
    causes = []
    if cfsr & (1 << 0): causes.append("IACCVIOL(指令访问违例)")
    if cfsr & (1 << 1): causes.append("DACCVIOL(数据访问违例)")
    if cfsr & (1 << 3): causes.append("MSTKERR(异常压栈)")
    if cfsr & (1 << 4): causes.append("MUNSTKERR(异常出栈)")
    if cfsr & (1 << 7): causes.append("MMFARVALID")
    if cfsr & (1 << 8): causes.append("IBUSERR(指令总线错误)")
    if cfsr & (1 << 9): causes.append("PRECISERR(精确数据总线错误)")
    if cfsr & (1 << 10): causes.append("IMPRECISERR(非精确总线错误)")
    if cfsr & (1 << 12): causes.append("STKERR(压栈错误)")
    if cfsr & (1 << 13): causes.append("LSPERR")
    if cfsr & (1 << 14): causes.append("UNSTKERR(出栈错误)")
    if cfsr & (1 << 15): causes.append("BFARVALID")
    if cfsr & (1 << 16): causes.append("UNDEFINSTR")
    if cfsr & (1 << 17): causes.append("INVSTATE")
    if cfsr & (1 << 18): causes.append("INVPC")
    if cfsr & (1 << 19): causes.append("NOCP")
    if cfsr & (1 << 24): causes.append("UNALIGNED")
    if cfsr & (1 << 25): causes.append("DIVBYZERO")
    return ", ".join(causes) if causes else "无"


def decode_bkp(mem, syms, out):
    bkp_base = 0x40002850
    regs = {i: mem.get(bkp_base + 4 * i, 0) for i in range(16)}
    magic = regs[BKP["MAGIC"]]
    if (magic & 0xFF) != 0x31:  # 'ERR1' 低字节
        out.append("BKP: 无有效崩溃记录（MAGIC 不匹配）")
        return
    src = regs[BKP["SRC"]]
    seq = regs[BKP["SEQ"]]
    pc = regs[BKP["PC"]]
    lr = regs[BKP["LR"]]
    addr = regs[BKP["ADDR"]]
    cfsr = regs[BKP["CFSR"]]
    hfsr = regs[BKP["HFSR"]]
    tick = regs[BKP["TICK"]]
    n0, n1, n2 = regs[BKP["NAME0"]], regs[BKP["NAME1"]], regs[BKP["NAME2"]]
    name = bytes([n0 & 0xFF, (n0 >> 8) & 0xFF, (n0 >> 16) & 0xFF, (n0 >> 24) & 0xFF,
                  n1 & 0xFF, (n1 >> 8) & 0xFF, (n1 >> 16) & 0xFF, (n1 >> 24) & 0xFF,
                  n2 & 0xFF, (n2 >> 8) & 0xFF, (n2 >> 16) & 0xFF, (n2 >> 24) & 0xFF]).split(b"\0")[0].decode("ascii", "replace")
    rapid = regs[BKP["RAPID"]]
    rtc = regs[BKP["RTC"]]
    out.append("=== BKP 崩溃摘要（err_mgr）===")
    out.append(f"  MAGIC=0x{magic:08X} 有效")
    out.append(f"  SRC={src} ({ERR_SRC[src] if src < len(ERR_SRC) else '?'})")
    out.append(f"  SEQ=#{seq}  快速崩溃计数={rapid}  RTC秒={rtc}")
    out.append(f"  PC=0x{pc:08X} -> {lookup(syms, pc)}")
    out.append(f"  LR=0x{lr:08X} -> {lookup(syms, lr)}")
    out.append(f"  FAULT_ADDR=0x{addr:08X}")
    out.append(f"  CFSR=0x{cfsr:08X} ({decode_cfsr(cfsr)})")
    out.append(f"  HFSR=0x{hfsr:08X} {'FORCED' if hfsr & (1 << 30) else ''}")
    out.append(f"  TICK={tick} ms  任务={name}")


def decode_icsr(icsr, out):
    vect = icsr & 0x1FF
    pend = (icsr >> 12) & 0x1FF
    out.append(f"=== ICSR=0x{icsr:08X} ===")
    out.append(f"  VECTACTIVE={vect}{' (无活跃异常)' if vect == 0 else f' -> IRQ{vect-16} {IRQ_TABLE.get(vect-16, '?')}'}")
    out.append(f"  VECTPENDING={pend}{' (无挂起)' if pend == 0 else f' -> IRQ{pend-16} {IRQ_TABLE.get(pend-16, '?')}'}")
    out.append(f"  ISRPENDING={1 if icsr & (1 << 22) else 0}  PENDSTSET={1 if icsr & (1 << 26) else 0}  PENDSVSET={1 if icsr & (1 << 28) else 0}")


def decode_nvic(mem, out):
    iser = [mem.get(0xE000E100 + 4 * i, 0) for i in range(3)]
    ispr = [mem.get(0xE000E200 + 4 * i, 0) for i in range(3)]
    out.append("=== NVIC 中断使能/挂起 ===")
    for bank in range(3):
        for bit in range(32):
            irq = bank * 32 + bit
            if irq > 81:
                break
            en = (iser[bank] >> bit) & 1
            pd = (ispr[bank] >> bit) & 1
            if pd:
                out.append(f"  IRQ{irq} {IRQ_TABLE.get(irq, '?'):18s} 使能={en} 挂起=1 !!!")
    # 汇总使能数
    en_cnt = sum(bin(x).count("1") for x in iser)
    out.append(f"  （使能中断总数={en_cnt}）")


def main():
    logdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
    if len(sys.argv) > 1:
        src = sys.argv[1]
    else:
        files = sorted(glob.glob(os.path.join(logdir, "forensic_*.txt")))
        if not files:
            print("没有 forensic 文件")
            return
        src = files[-1]
    print("分析:", src)
    # map 时间戳 vs 取证时间戳匹配性检查（布局漂移提示）
    try:
        mt = os.path.getmtime(MAP)
        ft = os.path.getmtime(src)
        if ft > mt + 60:
            print(f"!!! 警告：取证文件晚于 map 重建（{time.strftime('%m-%d %H:%M', time.localtime(mt))} vs "
                  f"{time.strftime('%m-%d %H:%M', time.localtime(ft))}）——符号化可能不匹配，"
                  f"请确认取证对应同一固件构建！")
    except OSError:
        pass
    syms = load_symbols(MAP)
    print(f"符号表: {len(syms)} 个函数")
    regs, mem = parse_forensic(src)

    out = []
    out.append(f"=== {os.path.basename(src)} 分析报告 ===")
    pc = regs.get("pc")
    lr = regs.get("lr")
    sp = regs.get("sp")
    out.append(f"PC=0x{pc:08X} -> {lookup(syms, pc)}" if pc is not None else "PC 缺失")
    out.append(f"LR=0x{lr:08X} -> {lookup(syms, lr)}" if lr is not None else "LR 缺失")
    out.append(f"SP=0x{sp:08X}  XPSR=0x{regs.get('xpsr', 0):08X}  CONTROL={regs.get('control', '?')}"
               f"  PRIMASK={regs.get('primask', '?')}  BASEPRI={regs.get('basepri', '?')}"
               f"  FAULTMASK={regs.get('faultmask', '?')}")
    if "xpsr" in regs:
        x = regs["xpsr"]
        out.append(f"  线程模式={1 if x & 0x1F == 0 else 0}（IPSR={x & 0x1F}） Thumb={1 if x & (1 << 24) else 0}")

    # tick
    uw = mem.get(0x20000048)
    xt = mem.get(0x2000005C)
    tcb = mem.get(0x20000050)
    out.append(f"uwTick={uw}  xTickCount={xt}  pxCurrentTCB=0x{tcb:08X}" if tcb else f"uwTick={uw}  xTickCount={xt}")

    # 任务名（TCB offset 52）
    if tcb and tcb in mem:
        name = b""
        for i in range(4):
            w = mem.get(tcb + 52 + i * 4, 0)
            name += bytes([w & 0xFF, (w >> 8) & 0xFF, (w >> 16) & 0xFF, (w >> 24) & 0xFF])
        name = name.split(b"\0")[0].decode("ascii", "replace")
        out.append(f"当前任务={name}")

    # 故障
    cfsr = mem.get(0xE000ED28)
    hfsr = mem.get(0xE000ED2C)
    if cfsr is not None:
        out.append(f"CFSR=0x{cfsr:08X} ({decode_cfsr(cfsr)})  HFSR=0x{hfsr:08X}")

    decode_icsr(mem.get(0xE000ED04, 0), out)
    decode_nvic(mem, out)

    # 栈符号化（PSP/MSP）
    for tag, stk_addr in (("PSP", sp), ("MSP", regs.get("msp"))):
        if stk_addr is None:
            continue
        out.append(f"=== {tag} 栈回溯（SP=0x{stk_addr:08X}）===")
        hits = 0
        for off in range(0, 64 * 4, 4):
            v = mem.get(stk_addr + off)
            if v is None:
                break
            if 0x08000000 <= v < 0x08100000:
                out.append(f"  [{off//4:3d}] 0x{stk_addr+off:08X}: 0x{v:08X} -> {lookup(syms, v)}")
                hits += 1
        if hits == 0:
            out.append("  （无 FLASH 返回地址）")

    decode_bkp(mem, syms, out)

    report = "\n".join(out)
    print(report)
    dst = os.path.join(logdir, "analyze_" + os.path.basename(src))
    with open(dst, "w", encoding="utf-8") as f:
        f.write(report + "\n")
    print(f"\n报告已保存: {dst}")


if __name__ == "__main__":
    main()
