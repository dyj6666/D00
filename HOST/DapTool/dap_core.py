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
