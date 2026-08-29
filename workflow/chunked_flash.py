# -*- coding: utf-8 -*-
"""chunked_flash.py —— 分块烧录（每块独立 OpenOCD 会话 + 延迟，规避 DAP HID 写超时）

用法：python workflow/chunked_flash.py <bin> <addr> [chunkKB] [delay_s]
"""
import subprocess
import sys
import time
import os

OPENOCD = r"D:\GIT-SPACE\D00\tools\xpack-openocd-0.12.0-7\bin\openocd.exe"
SCRIPTS = r"D:\GIT-SPACE\D00\tools\xpack-openocd-0.12.0-7\openocd\scripts"
TCL = r"D:\GIT-SPACE\D00\workflow\tcl"


def run_ocd(cmds, timeout=60):
    args = [OPENOCD, "-s", TCL, "-s", SCRIPTS,
            "-f", "interface/cmsis-dap.cfg", "-f", "target/stm32f4x.cfg",
            "-c", "adapter speed 1000"]
    for c in cmds:
        args += ["-c", c]
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        out = (r.stdout or "") + (r.stderr or "")
        return r.returncode, out
    except subprocess.TimeoutExpired:
        return -1, "TIMEOUT"


def main():
    binpath = sys.argv[1]
    addr = int(sys.argv[2], 16)
    chunk_kb = int(sys.argv[3]) if len(sys.argv) > 3 else 16
    delay_s = float(sys.argv[4]) if len(sys.argv) > 4 else 2.0

    data = open(binpath, "rb").read()
    size = len(data)
    print(f"烧录 {binpath} ({size}B) -> 0x{addr:X} chunk={chunk_kb}KB delay={delay_s}s")

    # 1) 擦除 APP 区（0x08010000 起 size 对齐）
    erase_start = addr
    erase_len = ((size + 0xFFFF) // 0x10000) * 0x10000
    rc, out = run_ocd(["init", "reset halt",
                       "mww 0x40003000 0x5555", "mww 0x40003004 0x6", "mww 0x40003008 0xFFF", "mww 0x40003000 0xAAAA",
                       f"flash erase_address 0x{erase_start:X} 0x{erase_len:X}",
                       "shutdown"], timeout=120)
    print(f"擦除 rc={rc}: {[l for l in out.splitlines() if 'erase' in l.lower() or 'Error' in l][-1:]}")
    if rc != 0:
        print("擦除失败")
        return

    # 2) 分块写（每块独立会话 + 延迟）
    flash_base = 0x08000000
    n_chunks = (size + chunk_kb * 1024 - 1) // (chunk_kb * 1024)
    for i in range(n_chunks):
        off = i * chunk_kb * 1024
        chunk = data[off:off + chunk_kb * 1024]
        tmp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs", f"flash_chunk_{i:02d}.bin")
        with open(tmp, "wb") as f:
            f.write(chunk)
        t0 = time.time()
        rc, out = run_ocd(["init", "halt",
                           f"flash write_bank 0 {tmp.replace(chr(92), '/')} 0x{addr + off - flash_base:X}",
                           "shutdown"], timeout=60)
        dt = time.time() - t0
        ok = "written" in out.lower() or rc == 0
        print(f"  块 {i+1}/{n_chunks} ({len(chunk)}B @0x{addr+off:X}) rc={rc} {dt:.1f}s {'OK' if rc==0 else 'FAIL'}")
        if rc != 0:
            print("  写块失败——停止")
            return
        time.sleep(delay_s)   # 探针缓冲排空，防 HID 写超时

    # 3) 校验
    rc, out = run_ocd(["init", "halt",
                       f"flash verify_bank 0 {binpath.replace(chr(92), '/')} 0x{addr - flash_base:X}",
                       "shutdown"], timeout=60)
    verified = "verified" in out.lower()
    print(f"校验 rc={rc} verified={verified}")
    # 4) 复位运行
    rc, out = run_ocd(["init", "reset run", "shutdown"], timeout=30)
    print(f"复位 rc={rc}")
    print("完成！")


if __name__ == "__main__":
    main()
