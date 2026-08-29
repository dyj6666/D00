# -*- coding: utf-8 -*-
"""MCU 全量版 v2 校验"""
import re

h = open(r"D:\GIT-SPACE\D00\_notes\mcu_structure_interactive.html", encoding="utf-8").read()
print("文件大小:", round(len(h) / 1024, 1), "KB")
views = len(re.findall(r"id='view-[a-z_0-9]+'", h)) + len(re.findall(r'id="view-[a-z_0-9]+"', h))
print("view 总数:", views, "(应为 45 = 总览1 + 子系统28 + 专题14 + 路线1 + 资源1)")
print("svg 总数:", len(re.findall(r"<svg", h)))
print("cbox 可点击:", len(re.findall(r'class="cbox"', h)) + len(re.findall(r"class='cbox'", h)))
print("data-code 代码实例:", len(re.findall(r"data-code=", h)))
print("data-note 抽屉:", len(re.findall(r"data-note=", h)))
print("note-btn 精讲徽标:", len(re.findall(r"class=.note-btn.", h)))
print("NOTES 注入:", h.count("var NOTES = {"))
print("github 链接:", len(re.findall(r"github\.com", h)))
print("总览分组数:", len(re.findall(r"内核与架构|外设控制|通信总线|系统与软件|工程与调试|硬件与联调", h)))
assert views == 51, f"view 应为 51(总览1 + 子系统34 + 专题14 + 路线1 + 资源1), 实际 {views}"
assert len(re.findall(r"<svg", h)) == 51
print("ALL_OK: MCU 全量版 51 视图就绪")
