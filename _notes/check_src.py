# -*- coding: utf-8 -*-
"""校验源码链接功能"""
import re

h = open(r"D:\GIT-SPACE\D00\_notes\linux_kernel_structure_interactive.html", encoding="utf-8").read()
print("data-src 源码链接条目:", len(re.findall(r"data-src=", h)))
print("📄 标记:", len(re.findall(r"📄", h)))
print("elixir 打开逻辑:", len(re.findall(r"elixir\.bootlin\.com/linux/latest/source/' \+ src", h)))
print("源码按钮:", len(re.findall(r"Elixir 源码", h)))
print("提示行:", len(re.findall(r"绿色=打开在线源码", h)))
srcs = re.findall(r'data-src="([^"]+)"', h)
print("示例链接:", srcs[:8])
print("链接总数正确性:", len(srcs) == len(re.findall(r"data-src=", h)))
