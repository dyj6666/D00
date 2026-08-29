# -*- coding: utf-8 -*-
"""MCU 版结构校验"""
import re

h = open(r"D:\GIT-SPACE\D00\_notes\mcu_structure_interactive.html", encoding="utf-8").read()
print("文件大小:", round(len(h) / 1024, 1), "KB")
print("svg 总数:", len(re.findall(r"<svg", h)))
views = len(re.findall(r"id='view-[a-z_0-9]+'", h)) + len(re.findall(r'id="view-[a-z_0-9]+"', h))
print("view 总数:", views)
print("cbox 可点击:", len(re.findall(r'class="cbox"', h)) + len(re.findall(r"class='cbox'", h)))
print("data-code 代码实例:", len(re.findall(r"data-code=", h)))
print("data-sym 符号:", len(re.findall(r"data-sym=", h)))
print("NOTES 注入:", h.count("var NOTES = {"))
print("精讲条目:", len(re.findall(r'"n_[a-z_]+": \{"', h)))
print("note-btn 徽标:", len(re.findall(r"class=.note-btn.", h)))
print("drawer:", len(re.findall(r'id="drawer"', h)))
print("github 源码链接:", len(re.findall(r"github\.com/STMicroelectronics|github\.com/FreeRTOS|github\.com/ARM-software", h)))
assert views == 22, f"view 应为 22(总览1+13子图+6专题+路线1+资源1), 实际 {views}"
assert len(re.findall(r"<svg", h)) == 22
print("ALL_OK: MCU 版 22 视图就绪")
