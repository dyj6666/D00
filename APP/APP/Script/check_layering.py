#!/usr/bin/env python3
"""架构分层守门：禁止越层 include（供 CI 与本地 pre-commit 使用）。

规则（见 Doc/ARCHITECTURE.md）：
  1. Application/SystemServices 禁止直接 include "stm32f4xx_hal.h"。
     - 白名单（平台模块/故障上下文，已在架构文档登记）：
       la_sample.c、err_mgr.c、signal_gen.c
  2. SystemServices/BSP 禁止 include Application 层头文件（向上依赖）。
     - 白名单：module.c（模块注册表 = 组合根，需知道全部模块）。
  3. main.h 是 CubeMX 总头（内含 HAL），命中记 WARNING（逐步收敛）。

用法：python check_layering.py [repo_root]
退出码：0=通过；1=违规。
"""

import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
APP = ROOT / "APP" / "APP"

HAL_INCLUDE = re.compile(r'#include\s+"stm32f4xx_hal\.h"')
MAIN_INCLUDE = re.compile(r'#include\s+"main\.h"')
APP_INCLUDE = re.compile(r'#include\s+"([A-Za-z0-9_]+)\.h"')

HAL_WHITELIST = {"la_sample.c", "err_mgr.c", "signal_gen.c"}
UPWARD_WHITELIST = {"module.c"}  # 组合根


def scan(paths, label, checker):
    errors = []
    warnings = []
    for p in paths:
        for f in p.glob("*.[ch]"):
            text = f.read_text(encoding="utf-8", errors="replace")
            for m in checker(f, text):
                if m[0] == "E":
                    errors.append(f"{label} {f.name}:{m[1]} {m[2]}")
                else:
                    warnings.append(f"{label} {f.name}:{m[1]} {m[2]}")
    return errors, warnings


def check_hal(f, text):
    for i, line in enumerate(text.splitlines(), 1):
        if HAL_INCLUDE.search(line) and f.name not in HAL_WHITELIST:
            yield ("E", i, line.strip())
        elif MAIN_INCLUDE.search(line):
            yield ("W", i, line.strip() + "  <-- main.h 含 HAL，建议改走 BSP")


def check_upward(f, text):
    if f.name in UPWARD_WHITELIST:
        return
    for i, line in enumerate(text.splitlines(), 1):
        m = APP_INCLUDE.search(line)
        if m and (APP / "Application" / (m.group(1) + ".h")).exists():
            yield ("E", i, line.strip() + "  <-- 向上依赖 Application")


def main():
    errs, warns = [], []
    e1, w1 = scan([APP / "Application", APP / "SystemServices"],
                  "[HAL]", check_hal)
    e2, w2 = scan([APP / "SystemServices", APP / "BSP"],
                  "[UP]", check_upward)
    errs += e1 + e2
    warns += w1 + w2
    for w in warns:
        print(f"WARN  {w}")
    for e in errs:
        print(f"ERROR {e}")
    print(f"\n{len(errs)} errors, {len(warns)} warnings")
    return 1 if errs else 0


if __name__ == "__main__":
    raise SystemExit(main())
