# -*- coding: utf-8 -*-
"""flash_recover.py —— 探针劣化环境下的稳健烧录（每步前软件重插 DAP）

流程：逐扇区擦除 → 分块写 → 校验（每步独立会话 + 软件重插）
"""
import subprocess
import sys
import time
import os

OPENOCD = r"D:\GIT-SPACE\D00\tools\xpack-openocd-0.12.0-7\bin\openocd.exe"
SCRIPTS = r"D:\GIT-SPACE\D00\tools\xpack-openocd-0.12.0-7\openocd\scripts"
TCL = r"D:\GIT-SPACE\D00\workflow\tcl"


def replug():
    """软件重插 DAP（Disable/Enable PnP）"""
    ps = ("$d = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | "
          "Where-Object { $_.InstanceId -match 'VID_C251' -and $_.Class -eq 'HIDClass' } | Select-Object -First 1; "
          "if ($d) { Disable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false -ErrorAction SilentlyContinue; "
          "Start-Sleep -Seconds 2; "
          "Enable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false -ErrorAction SilentlyContinue; "
          "Start-Sleep -Seconds 3 }")
    subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                   capture_output=True, timeout=30)


def run_ocd(cmds, timeout=60):
    args = [OPENOCD, "-s", TCL, "-s", SCRIPTS,
            "-f", "interface/cmsis-dap.cfg", "-f", "target/stm32f4x.cfg",
            "-c", "adapter speed 1000"]
    for c in cmds:
        args += ["-c", c]
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return r.returncode, (r.stdout or "") + (r.stderr or "")
    except subprocess.TimeoutExpired:
        return -1, "TIMEOUT"


def main():
    binpath = sys.argv[1]
    addr = int(sys.argv[2], 16)
    chunk_kb = int(sys.argv[3]) if len(sys.argv) > 3 else 16
    start_sec = int(sys.argv[4]) if len(sys.argv) > 4 else 4   # BOOT=0, APP=4

    data = open(binpath, "rb").read()
    size = len(data)
    print(f"稳健烧录 {os.path.basename(binpath)} ({size}B) -> 0x{addr:X}")

    # ---- 1) 逐扇区擦除（F407: 扇区0-3=16KB, 4=64KB, 5-11=128KB）----
    sectors = [start_sec]
    remaining = size
    sec_size = 0x4000 if start_sec < 4 else (0x10000 if start_sec == 4 else 0x20000)
    remaining -= sec_size
    for s in range(start_sec + 1, 12):
        if remaining <= 0:
            break
        sectors.append(s)
        remaining -= 0x20000
    print(f"擦除扇区: {sectors}")
    for sec in sectors:
        replug()
        rc, out = run_ocd(["init", "reset halt", f"flash erase_sector 0 {sec} {sec}", "shutdown"], 60)
        print(f"  扇区{sec}: {'OK' if rc == 0 else 'FAIL'}")
        if rc != 0:
            print("  擦除失败——重试一次")
            replug()
            rc, out = run_ocd(["init", "reset halt", f"flash erase_sector 0 {sec} {sec}", "shutdown"], 60)
            print(f"  重试扇区{sec}: {'OK' if rc == 0 else 'FAIL'}")
            if rc != 0:
                print("退出")
                return
        time.sleep(1)

    # ---- 2) 分块写 ----
    flash_base = 0x08000000
    n = (size + chunk_kb * 1024 - 1) // (chunk_kb * 1024)
    tmpdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
    print(f"分块写: {n} 块 x {chunk_kb}KB")
    for i in range(n):
        off = i * chunk_kb * 1024
        chunk = data[off:off + chunk_kb * 1024]
        tmp = os.path.join(tmpdir, f"fc_{i:02d}.bin")
        with open(tmp, "wb") as f:
            f.write(chunk)
        replug()
        rc, out = run_ocd(["init", "halt",
                           f"flash write_bank 0 {tmp.replace(chr(92), '/')} 0x{addr + off - flash_base:X}",
                           "shutdown"], 90)
        print(f"  块{i+1}/{n} (0x{addr+off:X}): {'OK' if rc == 0 else 'FAIL'}")
        if rc != 0:
            print("  写失败——重试一次")
            replug()
            rc, out = run_ocd(["init", "halt",
                               f"flash write_bank 0 {tmp.replace(chr(92), '/')} 0x{addr + off - flash_base:X}",
                               "shutdown"], 90)
            print(f"  重试块{i+1}: {'OK' if rc == 0 else 'FAIL'}")
            if rc != 0:
                print("退出")
                return
        time.sleep(2)

    # ---- 3) 校验 + 复位 ----
    replug()
    rc, out = run_ocd(["init", "halt",
                       f"flash verify_bank 0 {binpath.replace(chr(92), '/')} 0x{addr - flash_base:X}",
                       "shutdown"], 90)
    print(f"校验: {'OK' if 'verified' in out.lower() or rc == 0 else 'FAIL'}")
    replug()
    rc, out = run_ocd(["init", "reset run", "shutdown"], 30)
    print(f"复位运行: {'OK' if rc == 0 else 'FAIL'}")
    print("完成！")


if __name__ == "__main__":
    main()
