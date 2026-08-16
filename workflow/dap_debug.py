#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dap_debug - CMSIS-DAP hardware debug CLI (OpenOCD wrapper).

Epic debugging capability for the D00 project: read/write any memory or
peripheral register, set hardware breakpoints/watchpoints, decode the
active fault (CFSR/HFSR/BFAR + exception frame), resolve addresses to
symbols from the Keil map, and unwind stacks - WITHOUT adding printfs
or re-flashing repeatedly.

Every command starts a short-lived OpenOCD session (no persistent
connection), so it is safe to call from scripts or interactive use.

Usage:
  dap_debug.py halt                 halt the core
  dap_debug.py resume               resume execution
  dap_debug.py reset                reset-run the target
  dap_debug.py reg [pc|sp|lr|psp|msp|r0..r12|xpsr|all]
  dap_debug.py read  ADDR [N] [--w 16|32]     read memory words
  dap_debug.py write ADDR VALUE                write a word (mww)
  dap_debug.py read GPIOB_ODR                  register-name read
  dap_debug.py bp   ADDR [--len N] [--rw|--w] [--wait MS]
                                                set breakpoint, run and
                                                report whether it fired
  dap_debug.py rbp                             remove all breakpoints
  dap_debug.py fault                           decode active fault + frame
  dap_debug.py stack [--depth N]               unwind MSP/PSP frames
  dap_debug.py sym  ADDR                       address -> symbol
  dap_debug.py sym  NAME                       symbol -> address
  dap_debug.py pclist                          current PC + symbol
  dap_debug.py periph [NAME]                   list peripherals / regs
  dap_debug.py debug [--port N]                interactive live session
  dap_debug.py help

Addresses accept: 0x prefix, decimal, peripheral register names
(e.g. GPIOB_MODER, I2C1_CCR, SCB_CFSR), or symbol names from APP.map.
"""

import argparse
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(r"D:\GIT-SPACE\D00")
OPENOCD = REPO / "tools" / "xpack-openocd-0.12.0-7" / "bin" / "openocd.exe"
SCRIPTS = REPO / "tools" / "xpack-openocd-0.12.0-7" / "openocd" / "scripts"
APP_MAP = REPO / "APP" / "APP" / "MDK-ARM" / "APP" / "APP.map"
BOOT_MAP = REPO / "BOOT" / "BOOT" / "MDK-ARM" / "BOOT" / "BOOT.map"

# ----------------------------------------------------------------------
# Peripheral register map (name -> [base, {reg: offset}])
# ----------------------------------------------------------------------
GPIO_REGS = {
    "MODER": 0x00, "OTYPER": 0x04, "OSPEEDR": 0x08, "PUPDR": 0x0C,
    "IDR": 0x10, "ODR": 0x14, "BSRR": 0x18, "LCKR": 0x1C,
    "AFRL": 0x20, "AFRH": 0x24,
}
I2C_REGS = {
    "CR1": 0x00, "CR2": 0x04, "OAR1": 0x08, "OAR2": 0x0C, "DR": 0x10,
    "SR1": 0x14, "SR2": 0x18, "CCR": 0x1C, "TRISE": 0x20,
}
USART_REGS = {
    "SR": 0x00, "DR": 0x04, "BRR": 0x08, "CR1": 0x0C, "CR2": 0x10,
    "CR3": 0x14, "GTPR": 0x18,
}
SPI_REGS = {"CR1": 0x00, "CR2": 0x04, "SR": 0x08, "DR": 0x0C, "CRCPR": 0x10}
FSMC_REGS = {f"BCR{i}": 0x00 + (i - 1) * 8 for i in range(1, 5)}
FSMC_REGS.update({f"BTR{i}": 0x04 + (i - 1) * 8 for i in range(1, 5)})
RCC_REGS = {
    "CR": 0x00, "PLLCFGR": 0x04, "CFGR": 0x08, "CIR": 0x0C,
    "AHB1RSTR": 0x10, "AHB2RSTR": 0x14, "AHB3RSTR": 0x18,
    "APB1RSTR": 0x20, "APB2RSTR": 0x24,
    "AHB1ENR": 0x30, "AHB2ENR": 0x34, "AHB3ENR": 0x38,
    "APB1ENR": 0x40, "APB2ENR": 0x44,
    "AHB1LPENR": 0x50, "AHB2LPENR": 0x54, "AHB3LPENR": 0x58,
    "APB1LPENR": 0x60, "APB2LPENR": 0x64,
    "BDCR": 0x70, "CSR": 0x74,
}
SCB_REGS = {
    "CPUID": 0x00, "ICSR": 0x04, "VTOR": 0x08, "AIRCR": 0x0C, "SCR": 0x10,
    "CCR": 0x14, "SHPR1": 0x18, "SHPR2": 0x1C, "SHPR3": 0x20,
    "SHCSR": 0x24, "CFSR": 0x28, "HFSR": 0x2C, "DFSR": 0x30,
    "MMFAR": 0x34, "BFAR": 0x38, "AFSR": 0x3C,
}
TIM_REGS = {
    "CR1": 0x00, "CR2": 0x04, "SMCR": 0x08, "DIER": 0x0C, "SR": 0x10,
    "EGR": 0x14, "CCMR1": 0x18, "CCMR2": 0x1C, "CCER": 0x20,
    "CNT": 0x24, "PSC": 0x28, "ARR": 0x2C, "CCR1": 0x34, "CCR2": 0x38,
}
PERIPH = {
    "GPIOA": (0x40020000, GPIO_REGS), "GPIOB": (0x40020400, GPIO_REGS),
    "GPIOC": (0x40020800, GPIO_REGS), "GPIOD": (0x40020C00, GPIO_REGS),
    "GPIOE": (0x40021000, GPIO_REGS), "GPIOF": (0x40021400, GPIO_REGS),
    "GPIOG": (0x40021800, GPIO_REGS),
    "RCC": (0x40023800, RCC_REGS),
    "I2C1": (0x40005400, I2C_REGS), "I2C2": (0x40005800, I2C_REGS),
    "USART1": (0x40011000, USART_REGS), "USART2": (0x40004400, USART_REGS),
    "USART3": (0x40004800, USART_REGS),
    "SPI1": (0x40013000, SPI_REGS),
    "FSMC": (0xA0000000, FSMC_REGS),
    "TIM2": (0x40000000, TIM_REGS), "TIM3": (0x40000400, TIM_REGS),
    "TIM8": (0x40013400, TIM_REGS),
    "IWDG": (0x40003000, {"KR": 0x00, "PR": 0x04, "RLR": 0x08, "SR": 0x0C}),
    "CAN1": (0x40006400, {"MCR": 0x00, "MSR": 0x04, "TSR": 0x08, "RF0R": 0x0C,
                           "RF1R": 0x10, "IER": 0x14, "ESR": 0x18, "BTR": 0x1C}),
    "RTC": (0x40002800, {"TR": 0x00, "DR": 0x04, "CR": 0x08, "ISR": 0x0C,
                          "PRER": 0x10, "BKP0R": 0x50, "BKP1R": 0x54,
                          "BKP2R": 0x58, "BKP3R": 0x5C, "BKP4R": 0x60,
                          "BKP5R": 0x64, "BKP6R": 0x68, "BKP7R": 0x6C}),
    "DMA1": (0x40026000, {}), "DMA2": (0x40026400, {}),
    "EXTI": (0x40013C00, {"IMR": 0x00, "EMR": 0x04, "RTSR": 0x08,
                           "FTSR": 0x0C, "SWIER": 0x10, "PR": 0x14}),
    "SCB": (0xE000ED00, SCB_REGS),
    "SYSTICK": (0xE000E010, {"CTRL": 0x00, "LOAD": 0x04, "VAL": 0x08, "CALIB": 0x0C}),
}

# Word names for core registers
CORE_REGS = ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
             "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc", "xpsr",
             "msp", "psp", "primask", "basepri", "faultmask", "control"]


# ----------------------------------------------------------------------
# OpenOCD session helper
# ----------------------------------------------------------------------
DBGMCU_CR = 0xE0042004  # STM32F4 debug control register
# sleep/stop/standby(bit0-2) + IWDG(bit5=0x20) + WWDG(bit6=0x40) freeze on halt。
# 注意：0x1F 曾漏掉 bit5/bit6——IWDG 未冻结，halt 超过看门狗周期即复位
# （实测每次 DAP 会话后板子重启、启动流程 PC 采样污染）。0x7F 全冻结。 */
DBG_FREEZE_ALL = 0x7F


def run_ocd(commands, timeout=30):
    """Run a short OpenOCD session; return combined output text."""
    cmds = list(commands)
    # Release firmware runs with IWDG enabled (APP_DEBUG_MODE=0): halting the
    # core for longer than the watchdog period reboots the board mid-debug.
    # Freeze IWDG/WWDG/timers while halted so breakpoints never race the
    # watchdog. The write only takes effect while the core is halted.
    if "halt" in cmds:
        i = cmds.index("halt")
        cmds = cmds[:i + 1] + [f"mww 0x{DBGMCU_CR:X} {DBG_FREEZE_ALL}"] + cmds[i + 1:]
    elif "init" in cmds:
        i = cmds.index("init")
        cmds = cmds[:i + 1] + [f"mww 0x{DBGMCU_CR:X} {DBG_FREEZE_ALL}"] + cmds[i + 1:]
    args = ["-s", str(SCRIPTS),
            "-f", "interface/cmsis-dap.cfg",
            "-f", "target/stm32f4x.cfg",
            "-c", "adapter speed 500"]
    for c in cmds:
        args += ["-c", c]
    try:
        r = subprocess.run([str(OPENOCD)] + args, capture_output=True, text=True,
                           timeout=timeout)
        return r.returncode, (r.stdout or "") + "\n" + (r.stderr or "")
    except FileNotFoundError:
        print(f"[dap] OpenOCD not found: {OPENOCD}")
        sys.exit(2)
    except subprocess.TimeoutExpired:
        print("[dap] OpenOCD timed out")
        sys.exit(2)


def run_ocd_safe(commands, timeout=30):
    """Run a session that halts, captures data, then resumes before exit.

    A halt that outlives the session can wedge bus peripherals (e.g. I2C)
    on STM32F4, so inspection commands always hand control back to the
    running firmware once their data is captured.
    """
    return run_ocd(commands + ["resume", "shutdown"], timeout)


def parse_reg(output, name):
    """Extract `name (/32): 0x........` from OpenOCD output."""
    m = re.search(rf"{re.escape(name)}\s*\(/32\):\s*(0x[0-9a-fA-F]+)", output)
    if m:
        return int(m.group(1), 16)
    return None


def parse_mem(output, addr, n):
    """Extract words from `mdw` output lines.

    OpenOCD prints values without a 0x prefix, e.g.
        `0x40020414: 00004200 00000000 `
    Some versions print `0x00004200`; accept both forms.
    """
    out = {}
    for m in re.finditer(
            rf"(0x{addr:08x})\s*:\s*((?:(?:0x)?[0-9a-fA-F]{{8}}\s*)+)", output):
        words = [int(x, 16) for x in
                 re.findall(r"(?:0x)?([0-9a-fA-F]{8})", m.group(2))]
        base = int(m.group(1), 16)
        for i, w in enumerate(words):
            out[base + i * 4] = w
    return out


# ----------------------------------------------------------------------
# Symbol table from Keil map
# ----------------------------------------------------------------------
_SYM = None


def load_symbols():
    global _SYM
    if _SYM is not None:
        return _SYM
    _SYM = {}
    for mp in (APP_MAP, BOOT_MAP):
        if not mp.exists():
            continue
        for line in mp.read_text(encoding="utf-8", errors="ignore").splitlines():
            m = re.match(r"\s*([\w.]+)\s+0x(080[0-9a-fA-F]{5})\s+Thumb Code", line)
            if m:
                # Keil map addresses carry the Thumb bit (bit0=1); mask it so
                # breakpoints target the real even instruction address and
                # PC->symbol offsets are exact.
                name, addr = m.group(1), int(m.group(2), 16) & ~1
                _SYM.setdefault(addr, name)
    return _SYM


def sym_at(addr):
    # Symbol lookup only makes sense inside the firmware flash; RAM and
    # peripheral addresses must never be annotated with a stale flash symbol.
    if not (0x08000000 <= addr < 0x08100000):
        return None, None, 0
    syms = load_symbols()
    best = None
    for a, n in sorted(syms.items()):
        if a <= addr:
            best = (a, n)
        else:
            break
    if best:
        return best[0], best[1], addr - best[0]
    return None, None, 0


def sym_lookup(name):
    syms = load_symbols()
    return next(((a, n) for a, n in syms.items() if n == name), None)


# ----------------------------------------------------------------------
# Address resolution
# ----------------------------------------------------------------------
def resolve_addr(token):
    """Resolve a token to a 32-bit address: hex/decimal/periph reg/symbol."""
    token = token.strip()
    if not token:
        raise ValueError("empty address")
    # peripheral register: GPIOB_ODR, I2C1_CCR, SCB_CFSR, RCC_AHB1ENR
    m = re.match(r"([A-Za-z0-9]+)_([A-Za-z0-9]+)", token)
    if m:
        per, reg = m.group(1).upper(), m.group(2).upper()
        if per in PERIPH and reg in PERIPH[per][1]:
            return PERIPH[per][0] + PERIPH[per][1][reg], f"{per}_{reg}"
    if token.upper() in PERIPH:
        return PERIPH[token.upper()][0], token.upper()
    # symbol name
    hit = sym_lookup(token)
    if hit:
        return hit[0], token
    # numeric
    try:
        if token.lower().startswith("0x"):
            return int(token, 16), token
        return int(token, 10), token
    except ValueError:
        raise ValueError(f"cannot resolve address token: {token}")


# ----------------------------------------------------------------------
# Commands
# ----------------------------------------------------------------------
def cmd_halt():
    rc, out = run_ocd(["init", "halt", "reg pc", "reg sp", "reg lr",
                       "shutdown"])
    pc = parse_reg(out, "pc")
    sp = parse_reg(out, "sp")
    lr = parse_reg(out, "lr")
    if pc is not None:
        print(f"halted, pc=0x{pc:08X} sp=0x{sp:08X} lr=0x{lr:08X}")
        print("(one-shot session: core auto-resumes on disconnect; use "
              "`dap_debug.py debug` for a persistent halted session)")
    else:
        print(out)


def cmd_resume():
    rc, out = run_ocd(["init", "resume", "shutdown"])
    print("resumed" if rc == 0 else out)


def cmd_reset():
    rc, out = run_ocd(["init", "reset run", "shutdown"])
    print("reset-run" if rc == 0 else out)


def cmd_reg(names):
    wanted = names or ["pc", "sp", "lr"]
    wanted = [w.lower() for w in wanted]
    if "all" in wanted:
        wanted = CORE_REGS
    cmds = ["init", "halt"]
    for w in wanted:
        cmds.append("reg " + w)
    cmds.append("shutdown")
    rc, out = run_ocd_safe(cmds)
    vals = {}
    for w in wanted:
        v = parse_reg(out, w)
        if v is not None:
            vals[w] = v
    for w, v in vals.items():
        note = ""
        if w == "pc":
            a, n, off = sym_at(v)
            note = f"  <{n}+0x{off:X}>" if n else ""
        print(f"{w:8s} 0x{v:08X}{note}")


def cmd_read(addr_token, n, width):
    try:
        addr, label = resolve_addr(addr_token)
    except ValueError as e:
        print(f"[dap] {e}")
        sys.exit(2)
    cmds = ["init", "halt"]
    words = n
    if width == 16:
        for i in range(n):
            cmds.append(f"mdh 0x{addr + i*2:x} 1")
    elif width == 8:
        for i in range(n):
            cmds.append(f"mdb 0x{addr + i:x} 1")
    else:
        for i in range(0, n, 4):
            cmds.append(f"mdw 0x{addr + i:x} {min(4, n - i)}")
    rc, out = run_ocd_safe(cmds)
    if width == 32:
        vals = parse_mem(out, addr, n)
        for a in sorted(vals):
            a2, sname, off = sym_at(a)
            note = f"  <{sname}+0x{off:X}>" if sname else ""
            print(f"0x{a:08X}: 0x{vals[a]:08X}{note}")
    else:
        print(out)


def cmd_write(addr_token, value):
    try:
        addr, label = resolve_addr(addr_token)
    except ValueError as e:
        print(f"[dap] {e}")
        sys.exit(2)
    rc, out = run_ocd(["init", "halt", f"mww 0x{addr:x} 0x{int(value, 16):x}",
                       f"mdw 0x{addr:x} 1", "shutdown"])
    vals = parse_mem(out, addr, 1)
    if addr in vals:
        print(f"0x{addr:08X} = 0x{vals[addr]:08X}  (written {label})")
    else:
        print(out)


def cmd_bp(addr_token, length, mode, wait_ms):
    """Set a breakpoint/watchpoint and probe whether it fires.

    One short session: set bp -> resume -> wait -> halt -> report PC,
    then remove the breakpoint and hand control back to the firmware.
    Works as a "did this code path run?" probe without persistent state.
    """
    try:
        addr, label = resolve_addr(addr_token)
    except ValueError as e:
        print(f"[dap] {e}")
        sys.exit(2)
    op = "wp" if mode else "bp"
    cmds = ["init", "halt"]
    if mode:
        cmds.append(f"wp 0x{addr:x} {length} {mode}")
    else:
        cmds.append(f"bp 0x{addr:x} {length} hw")
    cmds.append("resume")
    cmds.append(f"sleep {wait_ms}")
    cmds.append("halt")
    cmds.append("reg pc")
    cmds.append("rbp all")
    cmds.append("resume")
    cmds.append("shutdown")
    # Long probes must not hit the 30 s session timeout.
    rc, out = run_ocd(cmds, timeout=max(30, wait_ms // 1000 + 15))
    pc = parse_reg(out, "pc")
    if pc is not None:
        a, n, off = sym_at(pc)
        # A breakpoint stops exactly at the address; a watchpoint stops at
        # whichever instruction triggered it, so "halted due to" is the hit
        # signal in that case.
        fired = "halted due to" in out.lower()
        hit = fired and ((addr <= pc < addr + length) or mode != "")
        if hit:
            print(f"HIT {op}@{label}: pc=0x{pc:08X}"
                  + (f" <{n}+0x{off:X}>" if n else ""))
        else:
            print(f"no hit in {wait_ms} ms: pc=0x{pc:08X}"
                  + (f" <{n}+0x{off:X}>" if n else "")
                  + "  (breakpoint removed, firmware resumed)")
    else:
        print(out)


def _telnet_read(sock, timeout=10):
    """Read until OpenOCD's `> ` prompt; return decoded text."""
    sock.settimeout(timeout)
    buf = b""
    while True:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        if buf.replace(b"\r\n", b"\n").endswith(b"> "):
            break
    # Strip telnet IAC negotiation (0xFF DO/WILL/DONT/WONT + option) and NULs
    # that some probes sprinkle into the stream.
    buf = re.sub(rb"\xff[\xfb-\xfe].", b"", buf)
    buf = buf.replace(b"\xff", b"").replace(b"\x00", b"")
    return buf.replace(b"\r\n", b"\n").decode(errors="replace")


def cmd_debug(port):
    """Interactive persistent OpenOCD session (telnet console).

    Breakpoints, watchpoints and halt state survive across commands here,
    which is the proper tool for step-by-step debugging without printf
    and without repeated flashing.
    """
    args = ["-s", str(SCRIPTS),
            "-f", "interface/cmsis-dap.cfg",
            "-f", "target/stm32f4x.cfg",
            "-c", "adapter speed 500",
            "-c", f"telnet_port {port}",
            "-c", "init", "-c", "halt",
            "-c", f"mww 0x{DBGMCU_CR:X} {DBG_FREEZE_ALL}"]
    proc = subprocess.Popen([str(OPENOCD)] + args,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    sock = None
    try:
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                time.sleep(0.2)
        if sock is None:
            print("[dap] cannot connect to OpenOCD telnet")
            return
        banner = _telnet_read(sock, timeout=5)
        print(banner, end="")
        print("== DAP interactive debugger (live OpenOCD session) ==")
        print("target halted. 常用: reg pc / mdw ADDR / bp ADDR / step /"
              " resume / halt / reset / rbp all / shutdown")
        while True:
            try:
                line = input("dap> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break
            if line in ("exit", "quit", "q"):
                break
            if not line:
                continue
            sock.sendall((line + "\n").encode())
            print(_telnet_read(sock), end="")
        try:
            sock.sendall(b"shutdown\n")
            _telnet_read(sock, timeout=3)
        except OSError:
            pass
    finally:
        if sock is not None:
            sock.close()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


def cmd_rbp():
    rc, out = run_ocd(["init", "halt", "rbp all", "shutdown"])
    print("all breakpoints/watchpoints removed" if rc == 0 else out)


def cmd_fault():
    cmds = ["init", "halt"]
    for r in ("pc", "lr", "sp", "msp", "psp"):
        cmds.append("reg " + r)
    for reg, name in (("SCB_CFSR", "CFSR"), ("SCB_HFSR", "HFSR"),
                      ("SCB_BFAR", "BFAR"), ("SCB_MMFAR", "MMFAR")):
        cmds.append("mdw 0x%X 1" % (0xE000ED00 + SCB_REGS[name]))
    cmds.append("shutdown")
    rc, out = run_ocd(cmds)
    cfsr = parse_mem(out, 0xE000ED28, 1).get(0xE000ED28)
    hfsr = parse_mem(out, 0xE000ED2C, 1).get(0xE000ED2C)
    bfar = parse_mem(out, 0xE000ED38, 1).get(0xE000ED38)
    mmfar = parse_mem(out, 0xE000ED34, 1).get(0xE000ED34)
    msp = parse_reg(out, "msp")
    psp = parse_reg(out, "psp")
    print("--- fault registers ---")
    if cfsr is not None:
        parts = []
        if cfsr & 0x00000002: parts.append("MMARVALID")
        if cfsr & 0x00000010: parts.append("MSTKERR")
        if cfsr & 0x00000080: parts.append("MMFSR: IACCVIOL")
        if cfsr & 0x00000200: parts.append("PRECISERR")
        if cfsr & 0x00000400: parts.append("IMPRECISERR")
        if cfsr & 0x00000800: parts.append("UNSTKERR")
        if cfsr & 0x00001000: parts.append("STKERR")
        if cfsr & 0x00008000: parts.append("BFARVALID")
        print(f"CFSR  0x{cfsr:08X}  [{', '.join(parts) or 'none'}]")
    if cfsr == 0 and hfsr == 0:
        print("=> 无活动故障（CFSR/HFSR 均为 0）；以下为历史残留或当前运行状态")
    if hfsr is not None:
        print(f"HFSR  0x{hfsr:08X}  [{'FORCED' if hfsr & 0x40000000 else ''}]")
    if bfar is not None and bfar:
        a, n, off = sym_at(bfar)
        print(f"BFAR  0x{bfar:08X}" + (f"  <{n}+0x{off:X}>" if n else ""))
    if mmfar is not None and mmfar:
        print(f"MMFAR 0x{mmfar:08X}")
    print(f"MSP   0x{msp:08X}   PSP   0x{psp:08X}")
    # Decode exception frame(s)
    for sp, tag in ((msp, "MSP"), (psp, "PSP")):
        if not sp:
            continue
        frame = parse_mem(out, sp, 8)
        if len(frame) < 8:
            # re-read frame via a fresh session for reliability
            rc2, out2 = run_ocd_safe(["init", "halt", f"mdw 0x{sp:x} 8"])
            frame = parse_mem(out2, sp, 8)
        if len(frame) >= 8:
            r0 = frame.get(sp + 0x00); r1 = frame.get(sp + 0x04)
            r2 = frame.get(sp + 0x08); r3 = frame.get(sp + 0x0C)
            r12 = frame.get(sp + 0x10); lr = frame.get(sp + 0x14)
            pc = frame.get(sp + 0x18); xpsr = frame.get(sp + 0x1C)
            a, n, off = sym_at(pc) if pc else (None, None, 0)
            print(f"--- {tag} exception frame ---")
            print(f"R0=0x{r0:08X} R1=0x{r1:08X} R2=0x{r2:08X} R3=0x{r3:08X}")
            print(f"R12=0x{r12:08X} LR=0x{lr:08X}")
            print(f"PC =0x{pc:08X}" + (f"  <{n}+0x{off:X}>" if n else "") + "  <-- crash point")
            print(f"xPSR=0x{xpsr:08X}")


def cmd_stack(depth):
    cmds = ["init", "halt"]
    for r in ("sp", "msp", "psp", "pc", "lr"):
        cmds.append("reg " + r)
    rc, out = run_ocd_safe(cmds)
    pc = parse_reg(out, "pc")
    sp = parse_reg(out, "sp")
    lr = parse_reg(out, "lr")
    print(f"pc=0x{pc:08X} sp=0x{sp:08X} lr=0x{lr:08X}")
    if not sp:
        return
    rc2, out2 = run_ocd_safe(["init", "halt", f"mdw 0x{sp:x} {depth}"])
    words = sorted(parse_mem(out2, sp, depth).items())
    for a, v in words:
        sname, off = "", 0
        if 0x08000000 <= v < 0x08100000:
            sa, sname, off = sym_at(v)
        note = f"  <{sname}+0x{off:X}>" if sname else ""
        print(f"0x{a:08X}: 0x{v:08X}{note}")


def cmd_sym(token):
    if token.lower().startswith("0x") or token.isdigit():
        a, n, off = sym_at(int(token, 16))
        if n:
            print(f"0x{int(token,16):08X} -> {n}+0x{off:X}")
        else:
            print(f"0x{int(token,16):08X} -> (no symbol)")
    else:
        hit = sym_lookup(token)
        if hit:
            print(f"{token} -> 0x{hit[0]:08X}")
        else:
            print(f"{token} -> (not found)")


def cmd_pclist():
    rc, out = run_ocd_safe(["init", "halt", "reg pc", "reg sp", "reg lr"])
    pc = parse_reg(out, "pc")
    sp = parse_reg(out, "sp")
    lr = parse_reg(out, "lr")
    a, n, off = sym_at(pc) if pc else (None, None, 0)
    print(f"pc=0x{pc:08X}" + (f" <{n}+0x{off:X}>" if n else ""))
    print(f"sp=0x{sp:08X}  lr=0x{lr:08X}")


def cmd_periph(name):
    if not name:
        for p, (base, regs) in sorted(PERIPH.items()):
            print(f"{p:8s} base=0x{base:08X}  ({len(regs)} regs)")
        return
    key = name.upper()
    if key not in PERIPH:
        print(f"[dap] unknown peripheral: {name}")
        sys.exit(2)
    base, regs = PERIPH[key]
    print(f"{key} base=0x{base:08X}")
    for r, off in sorted(regs.items(), key=lambda x: x[1]):
        print(f"  {key}_{r:8s} 0x{base + off:08X}")


def main():
    # Windows console defaults to GBK and cannot print UTF-8 diagnostics;
    # force UTF-8 so symbols like the OpenOCD banner never crash the CLI.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:
            pass
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help", "help"):
        print(__doc__)
        return
    cmd = sys.argv[1]
    args = sys.argv[2:]

    if cmd == "halt": cmd_halt()
    elif cmd == "resume": cmd_resume()
    elif cmd == "reset": cmd_reset()
    elif cmd == "reg": cmd_reg(args)
    elif cmd == "read":
        ap = argparse.ArgumentParser(add_help=False)
        ap.add_argument("addr"); ap.add_argument("n", nargs="?", type=int, default=1)
        ap.add_argument("--w", type=int, default=32, choices=[8, 16, 32])
        ns = ap.parse_args(args)
        cmd_read(ns.addr, ns.n, ns.w)
    elif cmd == "write":
        if len(args) < 2:
            print("usage: write ADDR VALUE"); sys.exit(2)
        cmd_write(args[0], args[1])
    elif cmd == "bp":
        ap = argparse.ArgumentParser(add_help=False)
        ap.add_argument("addr")
        ap.add_argument("--len", type=int, default=2)
        ap.add_argument("--rw", action="store_true")
        ap.add_argument("--w", action="store_true")
        ap.add_argument("--wait", type=int, default=2000)
        ns = ap.parse_args(args)
        mode = "w" if ns.w else ("rw" if ns.rw else "")
        cmd_bp(ns.addr, ns.len, mode, ns.wait)
    elif cmd == "rbp": cmd_rbp()
    elif cmd == "fault": cmd_fault()
    elif cmd == "stack":
        ap = argparse.ArgumentParser(add_help=False)
        ap.add_argument("--depth", type=int, default=16)
        ns = ap.parse_args(args)
        cmd_stack(ns.depth)
    elif cmd == "sym":
        if not args: print("usage: sym ADDR|NAME"); sys.exit(2)
        cmd_sym(args[0])
    elif cmd == "pclist": cmd_pclist()
    elif cmd == "periph": cmd_periph(args[0] if args else "")
    elif cmd == "debug":
        ap = argparse.ArgumentParser(add_help=False)
        ap.add_argument("--port", type=int, default=4444)
        ns = ap.parse_args(args)
        cmd_debug(ns.port)
    else:
        print(f"[dap] unknown command: {cmd}")
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
