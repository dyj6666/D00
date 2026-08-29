# -*- coding: utf-8 -*-
"""全量版结构校验"""
import re

h = open(r"D:\GIT-SPACE\D00\_notes\linux_kernel_structure_interactive.html", encoding="utf-8").read()
print("文件大小:", round(len(h) / 1024, 1), "KB")
print("svg 总数:", len(re.findall(r"<svg", h)))
print("view 总数:", len(re.findall(r"id='view-[a-z_0-9]+'", h)) + len(re.findall(r'id="view-[a-z_0-9]+"', h)))
print("deep 视图:", len(re.findall(r"id='view-deep_", h)))
print("roadmap:", len(re.findall(r"id='view-roadmap'", h)), "| resources:", len(re.findall(r"id='view-resources'", h)))
print("cbox 可点击:", len(re.findall(r'class="cbox"', h)) + len(re.findall(r"class='cbox'", h)))
print("showAt 面包屑:", len(re.findall(r"showAt\(", h)))
print("back():", len(re.findall(r"onclick=\"back\(\)\"", h)))
print("STAGE_SIZES:", len(re.findall(r"deep_eevdf: \[", h)))
print("render():", len(re.findall(r"\brender\(\);", h)))
views = len(re.findall(r"id='view-[a-z_0-9]+'", h)) + len(re.findall(r'id="view-[a-z_0-9]+"', h))
assert views == 43, f"view 数应为 43(总览1+子图13+专题10+地基枢纽1+地基11+地基专题5+路线1+资源1), 实际 {views}"
assert len(re.findall(r"<svg", h)) == 43
print("ALL_OK: 43 视图 = 总览1 + 子图13 + 专题10 + 地基枢纽1 + 地基11 + 地基专题5 + 路线1 + 资源1")
