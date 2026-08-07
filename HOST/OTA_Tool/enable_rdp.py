#!/usr/bin/env python3
"""启用 STM32F407 读保护（RDP Level 1）——生产发布前执行。

用法:
    python enable_rdp.py [COM] [baud]
    默认 COM13 / 115200

前置:
    1) 开发板 BOOT0 拨到 1 并复位（进入系统 bootloader，USART1）
    2) 板子经 CH340 连接 PC（即本脚本所用串口）

影响:
    - RDP Level 1：外部调试器（DAP/SWD/ST-LINK）无法读取 Flash 内容，
      固件防提取/防篡改（工业级部署必备）。
    - 解除 RDP 会触发全片擦除（恢复出厂态），方可重新烧录。
    - OTA 升级不受影响（BOOT/APP 内部读写 Flash 照常）。

验证:
    启用后执行一次 OTA 升级，确认 BOOT 校验/切换/启动确认全链路正常。
"""

import subprocess
import sys

CLI = (r"D:\STM32CUBECLT\STM32CubeCLT_1.18.0\STM32CubeProgrammer"
       r"\bin\STM32_Programmer_CLI.exe")


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM13"
    baud = sys.argv[2] if len(sys.argv) > 2 else "115200"

    cmd = [CLI, "-c", f"port={port}", f"br={baud}", "-ob", "RDP=0xBB"]
    print("执行:", " ".join(cmd))
    return subprocess.run(cmd).returncode


if __name__ == "__main__":
    raise SystemExit(main())
