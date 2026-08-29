# -*- coding: utf-8 -*-
"""v4 校验：NOTES 注入、抽屉、ⓘ 徽标"""
import re

h = open(r"D:\GIT-SPACE\D00\_notes\linux_kernel_structure_interactive.html", encoding="utf-8").read()
print("文件大小:", round(len(h) / 1024, 1), "KB")
print("NOTES 注入:", h.count("var NOTES = {"))
print("精讲条目数:", len(re.findall(r'"n_[a-z_]+": \{"', h)))
print("openNote 函数:", len(re.findall(r"function openNote", h)))
print("drawer 容器:", len(re.findall(r'id="drawer"', h)))
print("note-btn 徽标:", len(re.findall(r"class=.note-btn.", h)))
print("data-note 引用:", len(re.findall(r"data-note=", h)))
print("Bing 搜索链接:", len(re.findall(r"cn\.bing\.com/search", h)))
print("B站 搜索链接:", len(re.findall(r"search\.bilibili\.com/all", h)))
print("docs.kernel.org 精选:", len(re.findall(r"docs\.kernel\.org", h)))
print("LWN 链接:", len(re.findall(r"lwn\.net", h)))
print("LDD3:", len(re.findall(r"Kernel/LDD3", h)))
print("Elixir 源码链接:", len(re.findall(r"elixir\.bootlin\.com/linux/latest/source/[a-z]", h)))
nb = len(re.findall(r"class=.note-btn.", h))
assert nb >= 22, f"note-btn 应 >= 22, 实际 {nb}"
assert h.count("var NOTES = {") == 1
assert len(re.findall(r'id="drawer"', h)) >= 1
print("ALL_OK: 精讲抽屉系统就绪")
