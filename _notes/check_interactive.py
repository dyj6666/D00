# -*- coding: utf-8 -*-
"""校验交互版 HTML 的结构完整性"""
import re

h = open(r"D:\GIT-SPACE\D00\_notes\linux_kernel_structure_interactive.html", encoding="utf-8").read()
print("文件大小:", len(h))
print("cbox 可点击框:", len(re.findall(r'class="cbox"', h)))
print("subview 视图:", len(re.findall(r"class=.subview.", h)))
print("sub stages:", len(re.findall(r"id='stage-[a-z_]+'", h)))
print("view ids:", len(re.findall(r"id='view-[a-z_]+'", h)))
print("stage 舞台:", len(re.findall(r'class="stage"', h)))
print("attachZoom 函数定义:", len(re.findall(r"function attachZoom", h)))
print("attachZoom 调用:", len(re.findall(r"attachZoom\('stage-", h)))
print("STAGE_SIZES 注入:", len(re.findall(r"'overview': \[2600, 2160\]", h)))
print("showSub 函数:", len(re.findall(r"function showSub", h)))
print("showOverview 函数:", len(re.findall(r"function showOverview", h)))
print("Esc 返回:", len(re.findall(r"keydown", h)))
print("svg 总数:", len(re.findall(r"<svg", h)))
names = re.findall(r"SUB_NAMES = (\{.*?\})\n", h, re.S)
print("SUB_NAMES 定义:", bool(names))
print("OK" if all([len(re.findall(r'class="cbox"', h)) >= 13, len(re.findall(r"<svg", h)) >= 14]) else "HAS_ISSUE")
