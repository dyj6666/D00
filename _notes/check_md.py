# -*- coding: utf-8 -*-
"""校验 mermaid 源码：init 指令、classDef、class 赋值、花括号平衡"""
import re

for path in [r"D:\GIT-SPACE\D00\_notes\linux_kernel_structure.mmd",
             r"D:\GIT-SPACE\D00\_notes\linux_kernel_structure.md"]:
    t = open(path, encoding="utf-8").read()
    m = re.search(r"```mermaid\n(.*?)```", t, re.S) if path.endswith(".md") else re.search(r"(.*)", t, re.S)
    body = m.group(1)
    print(f"=== {path.split(chr(92))[-1]} ===")
    print("init 指令:", 1 if body.strip().startswith("%%{init") else 0)
    print("classDef:", len(re.findall(r"classDef \w+", body)))
    print("class 赋值:", len(re.findall(r"^\s+class \w+", body, re.M)))
    init = re.search(r"%%\{init: (.*?)\}%%", body, re.S)
    if init:
        js = init.group(1)
        print("init 花括号:", js.count("{"), js.count("}"), "| 方括号:", js.count("["), js.count("]"))
    print("节点:", len(re.findall(r"^\s+[A-Z0-9_]+\[", body, re.M)))
    print("subgraph:", body.count("subgraph"), "| end:", body.count("end"))
    print()
