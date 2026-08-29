# -*- coding: utf-8 -*-
"""校验 SUBS/DEEP 数据结构：列必须是 (str, [条目...])"""
import sys
sys.path.insert(0, r"D:\GIT-SPACE\D00\_notes")

src = open(r"D:\GIT-SPACE\D00\_notes\gen_interactive.py", encoding="utf-8").read()
# 提取 SUBS 与 DEEP 定义（粗略执行到数据定义为止不可行，改为正则摘取数据块后 eval）
import re, ast

def extract_dict(name):
    m = re.search(r'%s = \{(.*?)\n\}' % name, src, re.S)
    if not m:
        print(name, ": NOT FOUND"); return {}
    try:
        return ast.literal_eval("{" + m.group(1) + "\n}")
    except Exception as e:
        print(name, "literal_eval 失败:", e)
        return {}

SUBS = extract_dict("SUBS")
DEEP = extract_dict("DEEP")
for dname, D in (("SUBS", SUBS), ("DEEP", DEEP)):
    for key, (title, cols) in D.items():
        for ci, col in enumerate(cols):
            if not (isinstance(col, tuple) and len(col) == 2 and isinstance(col[1], list)):
                print(f"{dname}[{key}] 列{ci} 非法: {col!r}")
            else:
                for it in col[1]:
                    if not (isinstance(it, tuple) and len(it) in (2, 3)):
                        print(f"{dname}[{key}] 列{ci} 条目非法: {it!r}")
print("check done")
