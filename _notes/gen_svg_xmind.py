# -*- coding: utf-8 -*-
"""生成 Linux 内核知识结构图的 .xmind（XMind 可编辑）和 .svg（矢量图）"""
import json
import math
import uuid
import zipfile

OUT_XMIND = r"D:\GIT-SPACE\D00\_notes\linux_kernel_structure.xmind"
OUT_SVG = r"D:\GIT-SPACE\D00\_notes\linux_kernel_structure.svg"

# ============================ XMind ============================
# (主题, 子项列表)  子项为 str 或 (str, [子项])
def t(title, children=None):
    node = {"id": str(uuid.uuid4()), "class": "topic", "title": title}
    if children:
        node["children"] = {"attached": [t(*c) if isinstance(c, tuple) else t(c) for c in children]}
    return node

root = t("Linux 内核知识结构", [
    ("① 用户空间 User Space", [
        ("应用程序 / 服务", ["Apache · MySQL · GUI · 业务进程"]),
        ("标准 C 库", ["glibc / musl · 系统调用封装"]),
        ("Shell 与系统工具", ["bash · 常用命令 · 工具链"]),
    ]),
    ("② 系统调用接口 SCI", [
        "open · read · write · fork · execve · mmap · ioctl",
        "vDSO 快速路径 · 软中断陷入 (syscall)",
    ]),
    ("③ 内核核心 Kernel Core", [
        ("核心子系统", [
            ("进程管理", ["task_struct · 生命周期", "fork/exec · 线程 · namespace"]),
            ("进程调度", ["CFS · 实时调度", "负载均衡 · 调度类"]),
            ("内存管理", ["虚拟地址 · 页表 · MMU", "伙伴系统 · slab · OOM"]),
            ("文件系统", ["VFS · dentry/inode/file", "ext4/xfs · proc/sysfs · 回写"]),
            ("网络协议栈", ["socket · TCP/UDP/IP", "netfilter · 路由 · NAPI"]),
            ("进程间通信", ["管道 · 信号 · 信号量", "共享内存 · 消息队列 · futex"]),
            ("同步机制", ["自旋锁 · 互斥锁 · 读写锁", "RCU · 原子操作 · 内存屏障"]),
            ("中断与异常", ["上半部 / 下半部", "softirq · tasklet · workqueue"]),
            ("时间管理", ["jiffies · tick · clockevent", "hrtimer · NO_HZ 动态节拍"]),
            ("设备驱动", ["字符/块/网络驱动", "驱动模型 · platform · DMA · 设备树"]),
            ("内核模块", ["insmod / modprobe", "符号导出 · 模块加载器"]),
            ("虚拟化", ["KVM · QEMU", "cgroup · namespace（容器）"]),
        ]),
        ("横切关注点", [
            ("内核初始化", ["start_kernel → init 进程"]),
            ("调试与追踪", ["printk · ftrace · kprobe · eBPF · perf"]),
            ("构建与配置", ["Kconfig · Kbuild · 内核编码规范"]),
        ]),
    ]),
    ("④ 体系结构层 arch/", [
        "x86 / ARM / RISC-V",
        "汇编入口 · 启动流程",
        "MMU / 中断控制器底层",
    ]),
    ("⑤ 硬件层 Hardware", [
        "CPU · 内存 · 磁盘 · 网卡 · 外设",
        "中断控制器 · DMA · 总线 · 时钟",
    ]),
    ("学习路径（建议）", [
        "进程管理 + 调度 → CFS 原理",
        "内存管理 → 页表 · 伙伴系统 · slab",
        "同步机制 → 锁与 RCU 适用场景",
        "文件系统 + 块层 → VFS · 回写",
        "网络栈 → socket · TCP/IP · NAPI",
        "中断与时间 → 上下半部 · hrtimer",
        "实战工具 → ftrace · kprobe · perf · eBPF",
    ]),
])

content = [{
    "id": str(uuid.uuid4()),
    "class": "sheet",
    "title": "Linux 内核知识结构",
    "rootTopic": root,
}]
manifest = {"file-entries": [
    {"full-path": "content.json", "media-type": "application/json"},
    {"full-path": "metadata.json", "media-type": "application/json"},
]}
metadata = {"creator": {"name": "Xmind", "version": "24.1"}}

with zipfile.ZipFile(OUT_XMIND, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("content.json", json.dumps(content, ensure_ascii=False, indent=2))
    z.writestr("manifest.json", json.dumps(manifest, indent=2))
    z.writestr("metadata.json", json.dumps(metadata, indent=2))

# ============================ SVG ============================
FAM = "Microsoft YaHei, SimHei, sans-serif"
S = []
S.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 2600 1920" width="2600" height="1920">')
S.append('<rect width="2600" height="1920" fill="#FFFFFF"/>')

def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def srect(x, y, w, h, fill, stroke, sw=2, rx=12, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    S.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d}/>')

def stext(cx, cy, s, fs, fill="#1F1F1F", anchor="middle"):
    S.append(f'<text x="{cx}" y="{round(cy + fs * 0.35, 1)}" font-family="{FAM}" font-size="{fs}" fill="{fill}" text-anchor="{anchor}">{esc(s)}</text>')

def sltext(x, y, s, fs, fill="#1F1F1F"):
    stext(x, y, s, fs, fill, anchor="start")

def sarrow(x1, y1, x2, y2, stroke="#404040", sw=3, head=15, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    S.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{stroke}" stroke-width="{sw}"{d}/>')
    dx, dy = x2 - x1, y2 - y1
    L = math.hypot(dx, dy)
    ux, uy = dx / L, dy / L
    px, py = -uy, ux
    b1 = (x2 - ux * head + px * head * 0.55, y2 - uy * head + py * head * 0.55)
    b2 = (x2 - ux * head - px * head * 0.55, y2 - uy * head - py * head * 0.55)
    S.append(f'<polygon points="{x2},{y2} {b1[0]:.1f},{b1[1]:.1f} {b2[0]:.1f},{b2[1]:.1f}" fill="{stroke}"/>')

# 标题
stext(1300, 48, "Linux 内核知识结构框图", 46, "#17365D")
stext(1300, 98, "自底向上：硬件 → 体系结构 → 内核核心 → 系统调用 → 用户空间", 20, "#595959")

# ① 用户空间
LX, LW = 100, 2400
srect(LX, 130, LW, 180, "#F7F9FB", "#595959", 3, 16)
sltext(LX + 20, 142, "① 用户空间 User Space", 28, "#17365D")
usr = [("应用程序 / 服务", "Apache · MySQL · GUI · 业务进程"),
       ("标准 C 库", "glibc / musl · 系统调用封装"),
       ("Shell 与系统工具", "bash · 常用命令 · 工具链")]
for i, (n, d) in enumerate(usr):
    x = LX + 40 + i * 800
    srect(x, 185, 720, 105, "#DEEBF7", "#2E75B6")
    stext(x + 360, 223, n, 26)
    stext(x + 360, 263, d, 18)

# ② 系统调用
srect(950, 340, 700, 110, "#E2EFDA", "#538135")
stext(1300, 375, "② 系统调用接口 SCI", 26)
stext(1300, 412, "open · read · write · fork · execve · mmap · ioctl", 18)
stext(1300, 435, "vDSO 快速路径 · 软中断陷入 (syscall/int 0x80)", 18)
sarrow(1300, 314, 1300, 336)

# ③ 内核核心
srect(LX, 480, LW, 1050, "#F7F7F7", "#404040", 3, 16)
sltext(LX + 20, 492, "③ 内核核心 Kernel Core（内核态：Ring0 / EL1）", 28, "#17365D")
srect(140, 540, 2320, 730, "#FBFBFB", "#808080", 2, 12)
sltext(160, 550, "核心子系统", 24, "#595959")
core = [
    ("进程管理", "#FCE4D6", "#C55A11", ["task_struct · 生命周期", "fork/exec · 线程 · namespace"]),
    ("进程调度", "#FCE4D6", "#C55A11", ["CFS · 实时调度", "负载均衡 · 调度类"]),
    ("内存管理", "#FFF2CC", "#BF9000", ["虚拟地址 · 页表 · MMU", "伙伴系统 · slab · OOM"]),
    ("文件系统", "#E2EFDA", "#538135", ["VFS · dentry/inode/file", "ext4/xfs · proc/sysfs · 回写"]),
    ("网络协议栈", "#DDEBF7", "#2E75B6", ["socket · TCP/UDP/IP", "netfilter · 路由 · NAPI"]),
    ("进程间通信", "#EDEDED", "#7F7F7F", ["管道 · 信号 · 信号量", "共享内存 · 消息队列 · futex"]),
    ("同步机制", "#E4DFEC", "#7030A0", ["自旋锁 · 互斥锁 · 读写锁", "RCU · 原子操作 · 内存屏障"]),
    ("中断与异常", "#FBE5D6", "#C55A11", ["上半部 / 下半部", "softirq · tasklet · workqueue"]),
    ("时间管理", "#FFF2CC", "#BF9000", ["jiffies · tick · clockevent", "hrtimer · NO_HZ 动态节拍"]),
    ("设备驱动", "#D9E2F3", "#1F4E79", ["字符/块/网络驱动", "驱动模型 · platform · DMA · 设备树"]),
    ("内核模块", "#E2EFDA", "#538135", ["insmod / modprobe", "符号导出 · 模块加载器"]),
    ("虚拟化", "#DDEBF7", "#2E75B6", ["KVM · QEMU", "cgroup · namespace（容器）"]),
]
GX, GY, GW, GH, GAP = 160, 585, 560, 215, 15
for i, (n, fill, border, det) in enumerate(core):
    r, c = i // 4, i % 4
    x, y = GX + c * (GW + GAP), GY + r * (GH + GAP)
    srect(x, y, GW, GH, fill, border)
    stext(x + GW / 2, y + 56, n, 26)
    stext(x + GW / 2, y + 108, det[0], 18)
    stext(x + GW / 2, y + 148, det[1], 18)
# 横切关注点
srect(140, 1295, 2320, 210, "#FBFBFB", "#808080", 2, 12)
sltext(160, 1305, "横切关注点（贯穿所有子系统）", 24, "#595959")
xc = [("内核初始化", "start_kernel → init 进程"),
      ("调试与追踪", "printk · ftrace · kprobe · eBPF · perf"),
      ("构建与配置", "Kconfig · Kbuild · 内核编码规范")]
for i, (n, d) in enumerate(xc):
    x = 160 + i * 780
    srect(x, 1340, 740, 130, "#E1F5F5", "#00838F", 2, dash="14 9")
    stext(x + 370, 1387, n, 24)
    stext(x + 370, 1430, d, 18)
sarrow(1300, 454, 1300, 476)
sarrow(2380, 1268, 2380, 1299, dash="14 9")

# ④ 体系结构层
srect(LX, 1560, LW, 140, "#F7F9FB", "#595959", 3, 16)
sltext(LX + 20, 1572, "④ 体系结构层 arch/", 28, "#17365D")
srect(850, 1600, 900, 80, "#E2EFDA", "#538135")
stext(1300, 1630, "x86 / ARM / RISC-V", 24)
stext(1300, 1660, "汇编入口 · 启动流程 · MMU/中断控制器底层", 17)
sarrow(1300, 1534, 1300, 1556)

# ⑤ 硬件层
srect(LX, 1730, LW, 140, "#F7F9FB", "#595959", 3, 16)
sltext(LX + 20, 1742, "⑤ 硬件层 Hardware", 28, "#17365D")
srect(850, 1770, 900, 80, "#D9D9D9", "#595959")
stext(1300, 1800, "CPU · 内存 · 磁盘 · 网卡 · 外设", 24)
stext(1300, 1830, "中断控制器 · DMA · 总线 · 时钟", 17)
sarrow(1300, 1704, 1300, 1726)

S.append("</svg>")
with open(OUT_SVG, "w", encoding="utf-8") as f:
    f.write("\n".join(S))

print("xmind:", OUT_XMIND)
print("svg:", OUT_SVG)
