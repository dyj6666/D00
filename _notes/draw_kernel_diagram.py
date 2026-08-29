# -*- coding: utf-8 -*-
"""绘制 Linux 内核知识结构框图（离线 PIL 渲染，无网络依赖）"""
import math
from PIL import Image, ImageDraw, ImageFont

W, H = 2600, 1920
IMG = Image.new("RGB", (W, H), "#FFFFFF")
D = ImageDraw.Draw(IMG)

FW = "C:/Windows/Fonts/msyh.ttc"      # 微软雅黑
FWB = "C:/Windows/Fonts/msyhbd.ttc"   # 微软雅黑粗体
F_TITLE = ImageFont.truetype(FWB, 46)
F_SUB   = ImageFont.truetype(FW, 20)
F_LH    = ImageFont.truetype(FWB, 28)   # 层容器标题
F_SL    = ImageFont.truetype(FWB, 24)   # 子区域标题
F_NAME  = ImageFont.truetype(FW, 26)    # 框内主名
F_NAME2 = ImageFont.truetype(FW, 24)
F_DET   = ImageFont.truetype(FW, 18)    # 框内细节
F_DET2  = ImageFont.truetype(FW, 17)

INK = "#1F1F1F"

def ctext(cx, cy, s, f, fill=INK):
    """水平垂直居中绘制文本"""
    b = D.textbbox((0, 0), s, font=f)
    D.text((cx - (b[2] - b[0]) / 2, cy - (b[3] - b[1]) / 2), s, font=f, fill=fill)

def ltext(x, y, s, f, fill=INK):
    """左上对齐绘制文本"""
    D.text((x, y), s, font=f, fill=fill)

def rrect(x, y, w, h, fill, outline, width=2, r=12):
    D.rounded_rectangle([x, y, x + w, y + h], radius=r, fill=fill, outline=outline, width=width)

def dashed_line(x1, y1, x2, y2, fill, width=2, dash=14, gap=9):
    """沿直线绘制虚线（支持任意方向）"""
    dx, dy = x2 - x1, y2 - y1
    L = math.hypot(dx, dy)
    if L == 0:
        return
    ux, uy = dx / L, dy / L
    t = 0.0
    while t < L:
        e = min(t + dash, L)
        D.line([x1 + ux * t, y1 + uy * t, x1 + ux * e, y1 + uy * e], fill=fill, width=width)
        t = e + gap

def dashed_rect(x, y, w, h, fill, outline, width=2):
    D.rectangle([x, y, x + w, y + h], fill=fill)
    dashed_line(x, y, x + w, y, outline, width)
    dashed_line(x + w, y, x + w, y + h, outline, width)
    dashed_line(x + w, y + h, x, y + h, outline, width)
    dashed_line(x, y + h, x, y, outline, width)

def arrow(x1, y1, x2, y2, fill="#404040", width=3, head=15, dash=False):
    if dash:
        dashed_line(x1, y1, x2, y2, fill, width)
    else:
        D.line([x1, y1, x2, y2], fill=fill, width=width)
    dx, dy = x2 - x1, y2 - y1
    L = math.hypot(dx, dy)
    ux, uy = dx / L, dy / L
    px, py = -uy, ux                      # 垂直向量
    tip = (x2, y2)
    b1 = (x2 - ux * head + px * head * 0.55, y2 - uy * head + py * head * 0.55)
    b2 = (x2 - ux * head - px * head * 0.55, y2 - uy * head - py * head * 0.55)
    D.polygon([tip, b1, b2], fill=fill)

# ============================ 标题 ============================
ctext(W / 2, 48, "Linux 内核知识结构框图", F_TITLE, "#17365D")
ctext(W / 2, 98, "自底向上：硬件 → 体系结构 → 内核核心 → 系统调用 → 用户空间", F_SUB, "#595959")

# ============================ ① 用户空间 ============================
LX, LW = 100, 2400
USR_Y, USR_H = 130, 180
rrect(LX, USR_Y, LW, USR_H, "#F7F9FB", "#595959", 3, 16)
ltext(LX + 20, USR_Y + 12, "① 用户空间 User Space", F_LH, "#17365D")
usr_boxes = [
    ("应用程序 / 服务", "Apache · MySQL · GUI · 业务进程"),
    ("标准 C 库", "glibc / musl · 系统调用封装"),
    ("Shell 与系统工具", "bash · 常用命令 · 工具链"),
]
for i, (n, d) in enumerate(usr_boxes):
    x = LX + 40 + i * 800
    rrect(x, USR_Y + 55, 720, 105, "#DEEBF7", "#2E75B6", 2)
    ctext(x + 360, USR_Y + 93, n, F_NAME)
    ctext(x + 360, USR_Y + 133, d, F_DET)

# ============================ ② 系统调用 ============================
SCI_Y, SCI_H = 340, 110
rrect(950, SCI_Y, 700, SCI_H, "#E2EFDA", "#538135", 2)
ctext(1300, SCI_Y + 35, "② 系统调用接口 SCI", F_NAME)
ctext(1300, SCI_Y + 72, "open · read · write · fork · execve · mmap · ioctl", F_DET)
ctext(1300, SCI_Y + 95, "vDSO 快速路径 · 软中断陷入 (syscall/int 0x80)", F_DET)
arrow(1300, USR_Y + USR_H + 4, 1300, SCI_Y - 4)

# ============================ ③ 内核核心 ============================
K_Y, K_H = 480, 1050
rrect(LX, K_Y, LW, K_H, "#F7F7F7", "#404040", 3, 16)
ltext(LX + 20, K_Y + 12, "③ 内核核心 Kernel Core（内核态：Ring0 / EL1）", F_LH, "#17365D")

# --- 核心子系统 ---
CORE_X, CORE_Y, CORE_W, CORE_H = 140, 540, 2320, 730
rrect(CORE_X, CORE_Y, CORE_W, CORE_H, "#FBFBFB", "#808080", 2, 12)
ltext(CORE_X + 20, CORE_Y + 10, "核心子系统", F_SL, "#595959")

core_boxes = [
    ("进程管理",    "#FCE4D6", "#C55A11", ["task_struct · 生命周期", "fork/exec · 线程 · namespace"]),
    ("进程调度",    "#FCE4D6", "#C55A11", ["CFS · 实时调度", "负载均衡 · 调度类"]),
    ("内存管理",    "#FFF2CC", "#BF9000", ["虚拟地址 · 页表 · MMU", "伙伴系统 · slab · OOM"]),
    ("文件系统",    "#E2EFDA", "#538135", ["VFS · dentry/inode/file", "ext4/xfs · proc/sysfs · 回写"]),
    ("网络协议栈",  "#DDEBF7", "#2E75B6", ["socket · TCP/UDP/IP", "netfilter · 路由 · NAPI"]),
    ("进程间通信",  "#EDEDED", "#7F7F7F", ["管道 · 信号 · 信号量", "共享内存 · 消息队列 · futex"]),
    ("同步机制",    "#E4DFEC", "#7030A0", ["自旋锁 · 互斥锁 · 读写锁", "RCU · 原子操作 · 内存屏障"]),
    ("中断与异常",  "#FBE5D6", "#C55A11", ["上半部 / 下半部", "softirq · tasklet · workqueue"]),
    ("时间管理",    "#FFF2CC", "#BF9000", ["jiffies · tick · clockevent", "hrtimer · NO_HZ 动态节拍"]),
    ("设备驱动",    "#D9E2F3", "#1F4E79", ["字符/块/网络驱动", "驱动模型 · platform · DMA · 设备树"]),
    ("内核模块",    "#E2EFDA", "#538135", ["insmod / modprobe", "符号导出 · 模块加载器"]),
    ("虚拟化",      "#DDEBF7", "#2E75B6", ["KVM · QEMU", "cgroup · namespace（容器）"]),
]
GX, GY, GW, GH, GAP = 160, 585, 560, 215, 15
for i, (n, fill, border, det) in enumerate(core_boxes):
    r = i // 4
    c = i % 4
    x = GX + c * (GW + GAP)
    y = GY + r * (GH + GAP)
    rrect(x, y, GW, GH, fill, border, 2)
    ctext(x + GW / 2, y + 56, n, F_NAME)
    ctext(x + GW / 2, y + 108, det[0], F_DET)
    ctext(x + GW / 2, y + 148, det[1], F_DET)

# --- 横切关注点 ---
XC_X, XC_Y, XC_W, XC_H = 140, 1295, 2320, 210
rrect(XC_X, XC_Y, XC_W, XC_H, "#FBFBFB", "#808080", 2, 12)
ltext(XC_X + 20, XC_Y + 10, "横切关注点（贯穿所有子系统）", F_SL, "#595959")
xc_boxes = [
    ("内核初始化", "start_kernel → init 进程"),
    ("调试与追踪", "printk · ftrace · kprobe · eBPF · perf"),
    ("构建与配置", "Kconfig · Kbuild · 内核编码规范"),
]
for i, (n, d) in enumerate(xc_boxes):
    x = XC_X + 20 + i * 780
    dashed_rect(x, XC_Y + 45, 740, 130, "#E1F5F5", "#00838F", 2)
    ctext(x + 370, XC_Y + 92, n, F_NAME2)
    ctext(x + 370, XC_Y + 135, d, F_DET)

arrow(1300, SCI_Y + SCI_H + 4, 1300, K_Y - 4)
arrow(2380, CORE_Y + CORE_H - 2, 2380, XC_Y + 4, dash=True)

# ============================ ④ 体系结构层 ============================
AR_Y, AR_H = 1560, 140
rrect(LX, AR_Y, LW, AR_H, "#F7F9FB", "#595959", 3, 16)
ltext(LX + 20, AR_Y + 12, "④ 体系结构层 arch/", F_LH, "#17365D")
rrect(850, AR_Y + 40, 900, 80, "#E2EFDA", "#538135", 2)
ctext(1300, AR_Y + 70, "x86 / ARM / RISC-V", F_NAME2)
ctext(1300, AR_Y + 100, "汇编入口 · 启动流程 · MMU/中断控制器底层", F_DET2)
arrow(1300, K_Y + K_H + 4, 1300, AR_Y - 4)

# ============================ ⑤ 硬件层 ============================
HW_Y, HW_H = 1730, 140
rrect(LX, HW_Y, LW, HW_H, "#F7F9FB", "#595959", 3, 16)
ltext(LX + 20, HW_Y + 12, "⑤ 硬件层 Hardware", F_LH, "#17365D")
rrect(850, HW_Y + 40, 900, 80, "#D9D9D9", "#595959", 2)
ctext(1300, HW_Y + 70, "CPU · 内存 · 磁盘 · 网卡 · 外设", F_NAME2)
ctext(1300, HW_Y + 100, "中断控制器 · DMA · 总线 · 时钟", F_DET2)
arrow(1300, AR_Y + AR_H + 4, 1300, HW_Y - 4)

OUT = r"D:\GIT-SPACE\D00\_notes\linux_kernel_structure.png"
IMG.save(OUT)
print("saved:", OUT, IMG.size)
