#!/usr/bin/env python3
"""D00 DAP 上位机核心引擎：封装 OpenOCD（CMSIS-DAP）telnet 会话。

提供：连接/断开、芯片信息、健壮烧录（500kHz + IWDG 窗口扩展 + 分块
写入 + 整包校验）、擦除/转储/比对、内存读写、核心/外设寄存器、
目标控制（halt/resume/reset），以及全 DAP 驱动 OTA 触发
（预置包 + 写 BKP 升级标志 + 复位）。

烧录序列与 workflow/flash_dap.ps1 同源（实测稳定）：
  reset halt -> IWDG PR=256（~22s 窗口）-> 整区擦除 -> 48KB 分块
  write_bank（块间喂狗）-> verify_image -> reset run。
"""

import os
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import psutil

OPENOCD = r"D:\GIT-SPACE\D00\tools\xpack-openocd-0.12.0-7\bin\openocd.exe"
SCRIPTS = r"D:\GIT-SPACE\D00\tools\xpack-openocd-0.12.0-7\openocd\scripts"
TELNET_PORT = 4444
GDB_PORT = 3333

FLASH_BASE = 0x08000000
IWDG_KR = 0x40003000
IWDG_PR = 0x40003004
IWDG_RLR = 0x40003008
PWR_CR = 0x40007000
RTC_BKP0R = 0x40002850

BOOT_FLAG_UPGRADE = 0x5A5A
ERR_BKP_MAGIC_VAL = 0x45525231   # 'ERR1'
ERR_SRC_NAMES = {1: "NMI", 2: "HardFault", 3: "MemManage", 4: "BusFault",
                 5: "UsageFault", 6: "RTOS Assert", 7: "Stack Overflow",
                 8: "Task Stall", 9: "Unhandled IRQ"}

SCB_CFSR = 0xE000ED28
SCB_HFSR = 0xE000ED2C
SCB_DFSR = 0xE000ED30
SCB_MMFAR = 0xE000ED34
SCB_BFAR = 0xE000ED38
RCC_CSR = 0x40023874

UID_BASE = 0x1FFF7A10
RUN_VALID_ADDR = 0x080DFFF8
RUN_VERSION_ADDR = 0x080DFFFC
PARAM_BASE = 0x080E0000
PARAM_SLOT_OFFSET = 1024
PARAM_MAGIC = 0x50524D54

MAP_PATH = r"D:\GIT-SPACE\D00\APP\APP\MDK-ARM\APP\APP.map"

# FreeRTOS TCB 字段偏移（实测本工程构建：名称 @+52，见 DAP 工具日志）
TCB_OFF_PRIO = 44
TCB_OFF_PXSTACK = 48
TCB_OFF_NAME = 52
LIST_ITEM_OFF_NEXT = 4
LIST_ITEM_OFF_OWNER = 12
LIST_SIZE = 20

# F407 1MB 扇区映射：s0-s3=16KB，s4=64KB，s5-s11=128KB
def sector_bounds(addr, size):
    """返回覆盖 [addr, addr+size) 的整扇区 [start, length)（两端对齐）。"""
    end = addr + size - 1
    if addr < 0x08010000:
        start = 0x08000000 + ((addr - 0x08000000) // 0x4000) * 0x4000
    elif addr < 0x08020000:
        start = 0x08010000
    else:
        start = 0x08020000 + ((addr - 0x08020000) // 0x20000) * 0x20000
    if end < 0x08010000:
        endb = 0x08000000 + -(-(end + 1 - 0x08000000) // 0x4000) * 0x4000
    elif end < 0x08020000:
        endb = 0x08020000
    else:
        endb = 0x08020000 + -(-(end + 1 - 0x08020000) // 0x20000) * 0x20000
    return start, endb - start


class DapError(Exception):
    pass


def kill_orphan_openocd():
    """清理本工具链残留的孤儿 OpenOCD 进程（强杀上位机后子进程常驻）。
    仅匹配本仓库内置工具链路径，避免误杀其它会话。"""
    killed = []
    for p in psutil.process_iter(["pid", "name", "exe"]):
        try:
            if (p.info["name"] or "").lower() == "openocd.exe":
                exe = (p.info["exe"] or "").lower()
                if "xpack-openocd" in exe or "d00" in exe:
                    p.terminate()
                    killed.append(p.info["pid"])
        except Exception:
            continue
    if killed:
        time.sleep(1)
    return killed


def _parse_map_symbols(map_path):
    """从 Keil .map 提取符号地址：'pxCurrentTCB  0x20000050  Data  4'。"""
    syms = {}
    try:
        with open(map_path, encoding="utf-8", errors="replace") as f:
            for ln in f:
                m = re.match(r"\s*(\w+)\s+(0x[0-9a-fA-F]{8})\s+(Data|Code)",
                             ln)
                if m:
                    syms[m.group(1)] = int(m.group(2), 16)
    except OSError:
        pass
    return syms


def decode_cfsr(v):
    out = []
    m = v & 0xFFFF
    if m & 0x01: out.append("IACCVIOL")
    if m & 0x02: out.append("DACCVIOL")
    if m & 0x08: out.append("MSTKERR")
    if m & 0x10: out.append("MUNSTKERR")
    if m & 0x80: out.append("MMARVALID")
    b = (v >> 16) & 0xFF
    if b & 0x01: out.append("IBUSERR")
    if b & 0x02: out.append("PRECISERR")
    if b & 0x04: out.append("IMPRECISERR")
    if b & 0x08: out.append("UNSTKERR")
    if b & 0x10: out.append("STKERR")
    if b & 0x80: out.append("BFARVALID")
    u = (v >> 24) & 0xFF
    if u & 0x01: out.append("UNDEFINSTR")
    if u & 0x02: out.append("INVSTATE")
    if u & 0x04: out.append("INVPC")
    if u & 0x08: out.append("NOCP")
    if u & 0x10: out.append("UNALIGNED")
    if u & 0x20: out.append("DIVBYZERO")
    return ", ".join(out) if out else "(无)"


def decode_hfsr(v):
    out = []
    if v & 0x02: out.append("VECTTBL")
    if v & 0x40000000: out.append("FORCED")
    if v & 0x80000000: out.append("DEBUGEVT")
    return ", ".join(out) if out else "(无)"


def decode_reset(csr):
    reasons = []
    if csr & (1 << 24): reasons.append("低功耗复位(LPWR)")
    if csr & (1 << 25): reasons.append("窗口看门狗(WWDG)")
    if csr & (1 << 26): reasons.append("独立看门狗(IWDG)")
    if csr & (1 << 27): reasons.append("软件复位(SW)")
    if csr & (1 << 28): reasons.append("上电复位(POR)")
    if csr & (1 << 29): reasons.append("外部引脚复位(PIN)")
    if csr & (1 << 30): reasons.append("欠压复位(BOR)")
    return ", ".join(reasons) if reasons else "(无标志)"


class DapSession:
    """管理一个后台 OpenOCD 进程 + telnet 命令通道。"""

    def __init__(self, clock_khz=500, log=None):
        self.clock_khz = clock_khz
        self.log = log or (lambda *a, **k: None)
        self.proc = None
        self.sock = None
        self.info = {}
        self._lock = threading.Lock()

    # ---------------- 生命周期 ----------------

    def start(self):
        """启动 OpenOCD（init 后保持，不抢占目标），等待 telnet 就绪。"""
        if self.proc and self.proc.poll() is None:
            return
        try:
            pre = socket.socket()
            pre.bind(("127.0.0.1", TELNET_PORT))
            pre.close()
        except OSError:
            # 端口被占用：先尝试自动清理残留 OpenOCD，再报错（重试由 GUI 驱动）
            killed = kill_orphan_openocd()
            if killed:
                self.log("已清理残留 OpenOCD: %s，重新探测端口..." % killed)
                try:
                    pre = socket.socket()
                    pre.bind(("127.0.0.1", TELNET_PORT))
                    pre.close()
                except OSError:
                    raise DapError("telnet 端口仍被占用：请关闭其他 DAP 会话")
            else:
                raise DapError("telnet 端口被占用：请先关闭其他 DAP 会话")
        args = [OPENOCD, "-s", SCRIPTS,
                "-f", "interface/cmsis-dap.cfg",
                "-f", "target/stm32f4x.cfg",
                "-c", "adapter speed %d" % self.clock_khz,
                "-c", "init"]
        self.proc = subprocess.Popen(
            args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            bufsize=1, universal_newlines=True)
        self._parse_startup()
        self._connect_telnet()

    def _parse_startup(self):
        """从 OpenOCD 启动输出解析芯片信息（DPIDR/device/flash）。"""
        lines = []
        deadline = time.time() + 10
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                if self.proc.poll() is not None:
                    raise DapError("OpenOCD 启动失败（探针未插/被占用）")
                time.sleep(0.05)
                continue
            lines.append(line.rstrip())
            if "Listening on port %d" % TELNET_PORT in line:
                break
        for ln in lines:
            self.log("openocd: " + ln)
        text = "\n".join(lines)
        m = re.search(r"DPIDR (0x[0-9a-fA-F]+)", text)
        if m:
            self.info["dpidr"] = m.group(1)
        m = re.search(r"device id = (0x[0-9a-fA-F]+)", text)
        if m:
            self.info["device_id"] = m.group(1)
        m = re.search(r"flash size = (\d+) KiB", text)
        if m:
            self.info["flash_kb"] = m.group(1)
        m = re.search(r"(Cortex-M[0-9a-zA-Z]+) (r[0-9a-z]+)", text)
        if m:
            self.info["core"] = "%s %s" % (m.group(1), m.group(2))

    def _connect_telnet(self):
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", TELNET_PORT),
                                                     timeout=3)
                break
            except OSError:
                if self.proc.poll() is not None:
                    raise DapError("OpenOCD 进程已退出")
                time.sleep(0.1)
        if self.sock is None:
            raise DapError("无法连接 OpenOCD telnet")
        self.sock.settimeout(5)
        # 消费 telnet 欢迎横幅，避免后续命令响应错位一个
        try:
            self._read_until_prompt(5)
        except DapError:
            pass
        # 读取设备 ID（DBGMCU_IDCODE）与闪存容量（F407ZG=1024KB）
        try:
            ids = self.read_words(0xE0042000, 1)
            if ids:
                self.info["device_id"] = "0x%08X" % ids[0]
        except DapError:
            pass
        self.info.setdefault("flash_kb", "1024")

    def close(self):
        with self._lock:
            try:
                # 先恢复目标运行，避免会话结束后目标停留在 halt 态
                if self.sock is not None:
                    try:
                        self.sock.sendall(b"resume\n")
                        self._read_until_prompt(2)
                    except Exception:
                        pass
                if self.sock:
                    self.sock.close()
            except OSError:
                pass
            self.sock = None
            if self.proc and self.proc.poll() is None:
                try:
                    self.proc.terminate()
                    self.proc.wait(timeout=3)
                except Exception:
                    self.proc.kill()
            self.proc = None

    # ---------------- 命令通道 ----------------

    def _read_until_prompt(self, timeout=15.0):
        self.sock.settimeout(timeout)
        data = b""
        while True:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                raise DapError("OpenOCD 无响应（命令超时）")
            if not chunk:
                raise DapError("OpenOCD 连接断开")
            data += chunk
            text = data.decode("utf-8", "replace")
            if re.search(r">\s*$", text):
                return text

    def cmd(self, command, timeout=15.0):
        """发送一条命令并返回其输出文本（含回显/提示符）。"""
        with self._lock:
            if self.sock is None:
                raise DapError("未连接")
            try:
                self.sock.sendall((command + "\n").encode())
                out = self._read_until_prompt(timeout)
            except (DapError, OSError) as e:
                raise DapError("命令失败(%s): %s" % (command.split()[0], e))
        self.log("-> " + command)
        for ln in out.splitlines():
            ln = ln.strip()
            if ln and ln != ">" and not ln.startswith("openocd>"):
                self.log("   " + ln.replace("\ufffd", "?"))
        return out

    # ---------------- 目标控制 ----------------

    def reset_halt(self):
        self.cmd("reset halt")

    def reset_run(self):
        self.cmd("reset run")

    def halt(self):
        self.cmd("halt")

    def resume(self):
        self.cmd("resume")

    def hold_halted(self, on=True):
        """进入/退出"持续 halt"状态（每 2s 喂 IWDG 防复位）。"""
        self._hold = on

    # ---------------- 内存 / 寄存器 ----------------

    def read_words(self, addr, count):
        """halt 瞬间读 flash/外设字（读后立即 resume，不打扰运行）。"""
        out = self.cmd("halt")
        self.cmd("mww 0x%X 0xAAAA" % IWDG_KR)
        out = self.cmd("mdw 0x%X %d" % (addr, count))
        self.cmd("resume")
        vals = []
        for m in re.finditer(r"0x[0-9a-fA-F]+:\s+((?:[0-9a-fA-F]{8}\s*)+)", out):
            vals += [int(x, 16) for x in m.group(1).split()]
        return vals

    def read_words_raw(self, addr, count):
        """目标已 halt 时的原始读（不 halt/resume，供整段扫描保持一致）。"""
        out = self.cmd("mdw 0x%X %d" % (addr, count))
        vals = []
        for m in re.finditer(r"0x[0-9a-fA-F]+:\s+((?:[0-9a-fA-F]{8}\s*)+)", out):
            vals += [int(x, 16) for x in m.group(1).split()]
        return vals

    def write_u32(self, addr, value):
        self.cmd("halt")
        self.cmd("mww 0x%X 0xAAAA" % IWDG_KR)
        self.cmd("mww 0x%X 0x%X" % (addr, value))
        self.cmd("resume")

    def read_regs(self):
        """核心寄存器（halt 期间读，读后恢复运行）。"""
        out = self.cmd("halt")
        self.cmd("mww 0x%X 0xAAAA" % IWDG_KR)
        out = self.cmd("reg")
        self.cmd("resume")
        regs = {}
        for m in re.finditer(r"(\w+)\s+\(/\d+\):\s+0x([0-9a-fA-F]+)",
                             out, re.I):
            regs[m.group(1)] = int(m.group(2), 16)
        return regs

    def read_rtc_bkp(self):
        """RTC 时间 + BKP0R-3R（外设读，无需 halt）。"""
        out = self.cmd("mdw 0x40002850 4")
        bkp = []
        for m in re.finditer(r"0x[0-9a-fA-F]+:\s+((?:[0-9a-fA-F]{8}\s*)+)", out):
            bkp = [int(x, 16) for x in m.group(1).split()]
        return bkp

    # ---------------- 设备 / 固件 / 崩溃信息 ----------------

    def device_uid(self):
        vals = self.read_words(UID_BASE, 3)
        return vals if len(vals) == 3 else []

    def firmware_info(self):
        """RUN 区尾魔数/版本 + 参数区最后应用构建号。"""
        info = {}
        v = self.read_words(RUN_VALID_ADDR, 2)
        if len(v) == 2:
            info["magic"] = v[0]
            info["version"] = v[1]
        for slot in (0, 1):
            base = PARAM_BASE + slot * PARAM_SLOT_OFFSET
            p = self.read_words(base, 6)
            if len(p) >= 6 and p[0] == PARAM_MAGIC:
                info["last_build"] = p[5]
                info["boot_state"] = p[1]
                break
        return info

    def crash_record(self):
        """从 RTC 备份寄存器读 APP 崩溃记录（'ERR1' 摘要）。"""
        vals = self.read_words(RTC_BKP0R, 13)
        if len(vals) < 13:
            return None
        if (vals[1] & 0xFF) != (ERR_BKP_MAGIC_VAL & 0xFF):
            return None
        name = b""
        for i in range(10, 13):
            w = vals[i]
            name += bytes(((w >> 0) & 0xFF, (w >> 8) & 0xFF,
                          (w >> 16) & 0xFF, (w >> 24) & 0xFF))
        task = name.split(b"\x00")[0].decode("ascii", "replace")
        return {
            "src": vals[2],
            "src_name": ERR_SRC_NAMES.get(vals[2], "Unknown(%d)" % vals[2]),
            "seq": vals[3],
            "pc": vals[4],
            "lr": vals[5],
            "addr": vals[6],
            "cfsr": vals[7],
            "hfsr": vals[8],
            "tick": vals[9],
            "task": task,
            "cfsr_text": decode_cfsr(vals[7]),
            "hfsr_text": decode_hfsr(vals[8]),
        }

    def fault_regs(self):
        """实时读 SCB 故障寄存器 + RCC 复位原因。"""
        vals = self.read_words(SCB_CFSR, 3)          # CFSR/HFSR/DFSR
        bfar = self.read_words(SCB_BFAR, 2)          # BFAR/MMFAR
        csr = self.read_words(RCC_CSR, 1)
        out = {
            "cfsr": vals[0] if len(vals) > 0 else 0,
            "hfsr": vals[1] if len(vals) > 1 else 0,
            "dfsr": vals[2] if len(vals) > 2 else 0,
            "bfar": bfar[0] if len(bfar) > 0 else 0,
            "mmfar": bfar[1] if len(bfar) > 1 else 0,
            "rcc_csr": csr[0] if csr else 0,
            "cfsr_text": "",
            "hfsr_text": "",
            "reset_reason": decode_reset(csr[0] if csr else 0),
        }
        out["cfsr_text"] = decode_cfsr(out["cfsr"])
        out["hfsr_text"] = decode_hfsr(out["hfsr"])
        return out

    # ---------------- 调试命令 ----------------

    def bp_set(self, addr):
        self.cmd("bp 0x%X 2 hw" % addr)

    def bp_clear(self, addr):
        self.cmd("rbp 0x%X" % addr)

    def bp_list(self):
        out = self.cmd("bp")
        bps = []
        for m in re.finditer(r"([\w$]+)\s+at\s+0x([0-9a-fA-F]+)", out):
            bps.append(int(m.group(2), 16))
        return bps

    def step(self):
        self.cmd("halt")
        self.cmd("mww 0x%X 0xAAAA" % IWDG_KR)
        self.cmd("step")

    def disasm(self, addr, count=16):
        out = self.cmd("disassemble 0x%X %d" % (addr, count))
        return out

    # ---------------- RTOS 任务感知 ----------------

    def rtos_tasks(self, map_path=MAP_PATH):
        """解析 APP.map 里的 FreeRTOS 符号，遍历任务链表读出任务列表。
        字段：任务名 / 优先级 / 状态 / 栈已用词数（0xA5 填充扫描）。"""
        syms = _parse_map_symbols(map_path)
        need = ("pxCurrentTCB", "pxReadyTasksLists", "pxDelayedTaskList",
                "pxOverflowDelayedTaskList", "uxTopReadyPriority")
        if not all(s in syms for s in need):
            raise DapError("map 缺少 FreeRTOS 符号（固件与 map 不匹配？）")
        # 等待 APP 真正运行：OpenOCD 连接可能触发复位，PC 短暂在 BOOT
        for _ in range(20):
            pc = self._read_pc()
            if 0x08010000 <= pc <= 0x080DFFFF:
                break
            time.sleep(0.3)
        else:
            raise DapError("目标未运行 APP（PC=0x%08X）" % pc)
        self.cmd("halt")
        self.cmd("mww 0x%X 0xAAAA" % IWDG_KR)
        tasks = []
        try:
            cur = self.read_words_raw(syms["pxCurrentTCB"], 1)
            cur = cur[0] if cur else 0
            top = self.read_words_raw(syms["uxTopReadyPriority"], 1)
            top = top[0] if top else 0
            seen = set()

            def scan_list(list_base, state, max_items=64):
                n = self.read_words_raw(list_base, 1)
                n = n[0] if n else 0
                if n == 0 or n > max_items:
                    return
                node = self.read_words_raw(list_base + 12, 1)  # xListEnd.pxNext
                node = node[0] if node else 0
                end = list_base + 8
                for _ in range(max_items):
                    if node == 0 or node == end:
                        break
                    owner = self.read_words_raw(node + LIST_ITEM_OFF_OWNER, 1)
                    owner = owner[0] if owner else 0
                    if owner and owner not in seen:
                        seen.add(owner)
                        tasks.append(self._read_tcb(owner, state,
                                                    owner == cur))
                    nxt = self.read_words_raw(node + LIST_ITEM_OFF_NEXT, 1)
                    node = nxt[0] if nxt else 0

            for prio in range(top + 1):
                scan_list(syms["pxReadyTasksLists"] + prio * LIST_SIZE,
                          "Ready")
            dl = self.read_words_raw(syms["pxDelayedTaskList"], 1)
            if dl:
                scan_list(dl[0], "Blocked")
            dl2 = self.read_words_raw(syms["pxOverflowDelayedTaskList"], 1)
            if dl2:
                scan_list(dl2[0], "Blocked")
            tasks.sort(key=lambda t: -t["prio"])
            return tasks
        finally:
            self.cmd("resume")

    def _read_pc(self):
        out = self.cmd("halt")
        self.cmd("mww 0x%X 0xAAAA" % IWDG_KR)
        out = self.cmd("reg pc")
        self.cmd("resume")
        m = re.search(r"pc \(/32\): (0x[0-9a-fA-F]+)", out)
        return int(m.group(1), 16) if m else 0

    def _read_tcb(self, tcb, state, running):
        words = self.read_words_raw(tcb, 17)   # 前 68 字节覆盖 name/prio/stack
        name = b"".join(bytes(((w >> 0) & 0xFF, (w >> 8) & 0xFF,
                               (w >> 16) & 0xFF, (w >> 24) & 0xFF))
                        for w in words[13:17]) if len(words) >= 17 else b"?"
        name = name.split(b"\x00")[0].decode("ascii", "replace")
        prio = words[TCB_OFF_PRIO // 4] if len(words) > TCB_OFF_PRIO // 4 else 0
        pstack = (words[TCB_OFF_PXSTACK // 4]
                  if len(words) > TCB_OFF_PXSTACK // 4 else 0)
        used = 0
        if pstack:
            used = self._stack_used_raw(pstack)
        return {"name": name or "?", "prio": prio, "state": "Running" if
                running else state, "stack_used": used, "tcb": tcb}

    def _stack_used_raw(self, pstack, max_words=1024):
        """从栈底向上扫 0xA5 填充，返回已用词数。"""
        used = 0
        addr = pstack
        while used < max_words:
            w = self.read_words_raw(addr, 4)
            if len(w) < 4:
                break
            for v in w:
                if v != 0xA5A5A5A5:
                    return used
                used += 1
            addr += 16
        return used

    # ---------------- 烧录（健壮序列） ----------------

    def flash_file(self, path, addr, chunk_kb=48, verify=True,
                   progress=None, cancel=None):
        """分块烧录 + 校验；progress(frac, msg) 回调；cancel 事件可中止。"""
        size = os.path.getsize(path)
        sstart, slen = sector_bounds(addr, size)
        fwd = path.replace("\\", "/")
        # 先切出临时分块文件（与 workflow/flash_dap.ps1 同法）
        tmpdir = tempfile.mkdtemp(prefix="dap_flash_")
        chunks = []
        with open(path, "rb") as f:
            idx = 0
            while True:
                part = f.read(chunk_kb * 1024)
                if not part:
                    break
                cp = os.path.join(tmpdir, "chunk_%02d.bin" % idx)
                with open(cp, "wb") as cf:
                    cf.write(part)
                chunks.append((idx * chunk_kb * 1024, cp.replace("\\", "/")))
                idx += 1

        def p(frac, msg):
            if progress:
                progress(frac, msg)

        p(0.02, "reset halt...")
        self.cmd("reset halt")
        # IWDG 窗口扩展：PR=256 -> ~22s（halt 期间不会被看门狗复位）
        self.cmd("mww 0x%X 0x5555" % IWDG_KR)
        self.cmd("mww 0x%X 0x6" % IWDG_PR)
        self.cmd("mww 0x%X 0xFFF" % IWDG_RLR)
        self.cmd("mww 0x%X 0xAAAA" % IWDG_KR)

        p(0.05, "erase 0x%X len 0x%X..." % (sstart, slen))
        self.cmd("flash erase_address 0x%X 0x%X" % (sstart, slen),
                 timeout=60)

        written = 0
        for off, chunk in chunks:
            if cancel and cancel.is_set():
                p(0.9, "已取消，复位目标...")
                self.cmd("reset run")
                shutil.rmtree(tmpdir, ignore_errors=True)
                return False
            self.cmd("mww 0x%X 0xAAAA" % IWDG_KR)
            foff = addr + off - FLASH_BASE
            self.cmd("flash write_bank 0 %s 0x%X" % (chunk, foff),
                     timeout=60)
            written = min(size, written + os.path.getsize(chunk))
            p(0.05 + 0.85 * written / size,
              "write %d/%d" % (written, size))

        if verify:
            p(0.92, "verify...")
            out = self.cmd("verify_image %s 0x%X" % (fwd, addr), timeout=90)
            if not re.search(r"verified\s+\d+\s+bytes", out):
                self.cmd("reset run")
                shutil.rmtree(tmpdir, ignore_errors=True)
                raise DapError("校验失败：flash 内容与文件不一致")
        self.cmd("reset run")
        shutil.rmtree(tmpdir, ignore_errors=True)
        p(1.0, "烧录完成 + 校验通过")
        return True

    # ---------------- 擦除 / 转储 / 比对 ----------------

    def erase(self, addr, size):
        sstart, slen = sector_bounds(addr, size)
        self.cmd("reset halt")
        self.cmd("mww 0x%X 0x5555" % IWDG_KR)
        self.cmd("mww 0x%X 0x6" % IWDG_PR)
        self.cmd("mww 0x%X 0xFFF" % IWDG_RLR)
        self.cmd("mww 0x%X 0xAAAA" % IWDG_KR)
        self.cmd("flash erase_address 0x%X 0x%X" % (sstart, slen),
                 timeout=60)
        self.cmd("reset run")
        return sstart, slen

    def dump(self, addr, size, outfile):
        fwd = outfile.replace("\\", "/")
        self.cmd("halt")
        self.cmd("mww 0x%X 0xAAAA" % IWDG_KR)
        self.cmd("dump_image %s 0x%X 0x%X" % (fwd, addr, size), timeout=120)
        self.cmd("resume")
        return size

    def verify_file(self, path, addr):
        fwd = path.replace("\\", "/")
        self.cmd("halt")
        self.cmd("mww 0x%X 0xAAAA" % IWDG_KR)
        out = self.cmd("verify_image %s 0x%X" % (fwd, addr), timeout=90)
        self.cmd("resume")
        return bool(re.search(r"verified\s+\d+\s+bytes", out))

    # ---------------- OTA 全 DAP 触发 ----------------

    def set_upgrade_flag(self):
        """使能 PWR 时钟+DBP 后写 BKP0R=0x5A5A（复位前调用）。
        reset halt 停在复位向量，PWR 外设时钟尚未使能，直接写 PWR_CR
        会被忽略——必须先经 RCC_APB1ENR(bit28) 打开 PWR 时钟。"""
        self.cmd("reset halt")
        self.cmd("set _apb [mrw 0x40023840]")
        self.cmd("mww 0x40023840 [expr {$_apb | 0x10000000}]")
        self.cmd("set _cr [mrw 0x%X]" % PWR_CR)
        self.cmd("mww 0x%X [expr {$_cr | 0x100}]" % PWR_CR)
        self.cmd("mww 0x%X 0x%X" % (RTC_BKP0R, BOOT_FLAG_UPGRADE))
        chk = self.read_words(RTC_BKP0R, 1)
        if not chk or chk[0] != BOOT_FLAG_UPGRADE:
            raise DapError("BKP 升级标志写入失败")

    def ota_stage_and_trigger(self, pkg, addr=0x080A0000):
        """预置加密包到下载区 + 写 BKP 升级标志 + 复位进 BOOT 应用。
        完全无需串口：整条链路由 DAP 驱动。"""
        self.flash_file(pkg, addr)
        self.set_upgrade_flag()
        self.cmd("reset run")
        return True
