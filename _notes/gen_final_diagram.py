# -*- coding: utf-8 -*-
"""生成终极版 Linux 内核知识结构图：linux_kernel_structure.svg + linux_kernel_structure.html
HTML 内嵌矢量 SVG（离线可用，支持图上滚轮缩放/拖拽平移）+ Mermaid 实时渲染（联网时加载 CDN）"""
import html as H

W, H_ = 2600, 2160
FAM = "Microsoft YaHei, SimHei, sans-serif"
S = []
S.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H_}" width="{W}" height="{H_}">')
S.append(f'<rect width="{W}" height="{H_}" fill="#FFFFFF"/>')

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
    import math
    dx, dy = x2 - x1, y2 - y1
    L = math.hypot(dx, dy)
    ux, uy = dx / L, dy / L
    px, py = -uy, ux
    b1 = (x2 - ux * head + px * head * 0.55, y2 - uy * head + py * head * 0.55)
    b2 = (x2 - ux * head - px * head * 0.55, y2 - uy * head - py * head * 0.55)
    S.append(f'<polygon points="{x2},{y2} {b1[0]:.1f},{b1[1]:.1f} {b2[0]:.1f},{b2[1]:.1f}" fill="{stroke}"/>')

# ============ 标题 ============
stext(W / 2, 48, "Linux 内核知识结构图（终极版）", 46, "#17365D")
stext(W / 2, 98, "自底向上：硬件 → 体系结构 → 内核核心 → 系统调用 → 用户空间", 20, "#595959")

# ============ ① 用户空间 ============
LX, LW = 100, 2400
srect(LX, 130, LW, 180, "#F7F9FB", "#595959", 3, 16)
sltext(LX + 20, 142, "① 用户空间 User Space", 28, "#17365D")
usr = [("应用程序 / 服务", "Apache · MySQL · GUI"),
       ("标准 C 库", "glibc / musl · 系统调用封装"),
       ("Shell 与系统工具", "bash · 常用命令 · 工具链"),
       ("运行时", "JVM / 语言运行时 / 容器编排")]
for i, (n, d) in enumerate(usr):
    x = LX + 40 + i * 590
    srect(x, 185, 560, 105, "#DEEBF7", "#2E75B6")
    stext(x + 280, 223, n, 26)
    stext(x + 280, 263, d, 18)

# ============ ② 系统调用 ============
srect(950, 340, 700, 110, "#E2EFDA", "#538135")
stext(1300, 375, "② 系统调用接口 SCI + vDSO", 26)
stext(1300, 412, "open · read · write · fork · execve · mmap · ioctl · epoll · io_uring", 18)
stext(1300, 435, "vDSO 快速路径 · 软中断陷入 (syscall / int 0x80)", 18)
sarrow(1300, 314, 1300, 336)

# ============ ③ 内核核心 ============
srect(LX, 480, LW, 1330, "#F7F7F7", "#404040", 3, 16)
sltext(LX + 20, 492, "③ 内核核心 Kernel Core（内核态：Ring0 / EL1）", 28, "#17365D")

groups = [
    ("进程与调度",  "#FCE4D6", "#C55A11", [
        ("进程管理", "task_struct · fork/exit · 线程 · namespace"),
        ("调度器", "EEVDF(CFS) · 实时/Deadline · 负载均衡 · PREEMPT_RT")]),
    ("内存管理",    "#FFF2CC", "#BF9000", [
        ("虚拟内存", "页表 · MMU · 缺页 · COW"),
        ("物理内存", "伙伴系统 · slab/slub · CMA · HugePage"),
        ("页缓存与回收", "page cache · swap/zram · KSM · OOM")]),
    ("文件系统与存储", "#E2EFDA", "#538135", [
        ("VFS 抽象", "dentry · inode · file · super_block"),
        ("具体文件系统", "ext4 · xfs · btrfs · proc/sysfs · tmpfs"),
        ("块层与 IO", "bio · I/O 调度 · io_uring · DMA")]),
    ("网络协议栈",  "#DDEBF7", "#2E75B6", [
        ("网络分层", "socket · TCP/UDP · IP · 邻居子系统"),
        ("转发与过滤", "netfilter/iptables · TC · 路由"),
        ("高速数据面", "NAPI · XDP · eBPF · DPDK")]),
    ("并发与通信",  "#E4DFEC", "#7030A0", [
        ("进程间通信", "管道 · 信号 · SysV · futex · socket"),
        ("同步机制", "原子 · 自旋锁 · 互斥锁 · RCU · seqlock · 内存屏障")]),
    ("中断与时间",  "#FBE5D6", "#C55A11", [
        ("中断子系统", "上半部/下半部 · softirq · workqueue · threaded IRQ"),
        ("时间管理", "jiffies · hrtimer · clockevent · NO_HZ")]),
    ("驱动与虚拟化", "#D9E2F3", "#1F4E79", [
        ("设备驱动", "字符/块/网络 · 驱动模型 · 设备树/ACPI · DMA"),
        ("虚拟化与容器", "KVM · virtio · vfio · cgroup v2 · namespace")]),
    ("安全与电源",  "#EDEDED", "#7F7F7F", [
        ("安全框架", "LSM · SELinux/AppArmor · lockdown · 密钥环"),
        ("电源管理", "cpuidle · cpufreq · runtime PM · suspend")]),
]
COL_X, COL_W, GAPX = 140, 1130, 60
ROW_Y, ROW_H = 570, 215
for gi, (gname, fill, border, items) in enumerate(groups):
    col = gi % 2
    row = gi // 2
    x = COL_X + col * (COL_W + GAPX)
    y = ROW_Y + row * (ROW_H + 20)
    srect(x, y, COL_W, ROW_H, fill, border, 2.5)
    sltext(x + 16, y + 30, gname, 22, border)
    total = len(items) * 48 + (len(items) - 1) * 10
    sy = y + 44 + (208 - total) // 2
    for (n, d) in items:
        srect(x + 14, sy, COL_W - 28, 48, "#FFFFFF", "#BFBFBF", 1.5, 8)
        stext(x + 30, sy + 26, n, 21, "#1F1F1F", anchor="start")
        stext(x + COL_W - 30, sy + 26, d, 17, "#404040", anchor="end")
        sy += 58

# 横切关注点
srect(140, 1630, 2320, 170, "#FBFBFB", "#808080", 2, 12)
sltext(160, 1642, "横切关注点（贯穿所有子系统）", 24, "#595959")
xc = [("内核初始化", "start_kernel → init 进程"),
      ("调试与观测", "printk · ftrace · kprobe · perf · eBPF/BTF · kgdb · KUnit"),
      ("构建与开发", "Kconfig · Kbuild · LLVM/Clang · 编码规范")]
for i, (n, d) in enumerate(xc):
    x = 160 + i * 780
    srect(x, 1670, 740, 110, "#E1F5F5", "#00838F", 2, dash="14 9")
    stext(x + 370, 1715, n, 24)
    stext(x + 370, 1755, d, 17)
sarrow(1300, 454, 1300, 476)
sarrow(2380, 1479, 2380, 1626, dash="14 9")

# ============ ④ 体系结构层 ============
srect(LX, 1830, LW, 140, "#F7F9FB", "#595959", 3, 16)
sltext(LX + 20, 1842, "④ 体系结构层 arch/", 28, "#17365D")
srect(850, 1870, 900, 80, "#E2EFDA", "#538135")
stext(1300, 1900, "x86 / ARM / RISC-V", 24)
stext(1300, 1930, "启动汇编 · 页表/MMU · APIC/GIC · 原子与屏障实现", 17)
sarrow(1300, 1814, 1300, 1826)

# ============ ⑤ 硬件层 ============
srect(LX, 1990, LW, 140, "#F7F9FB", "#595959", 3, 16)
sltext(LX + 20, 2002, "⑤ 硬件层 Hardware", 28, "#17365D")
srect(850, 2030, 900, 80, "#D9D9D9", "#595959")
stext(1300, 2060, "CPU · 内存 · 磁盘 · 网卡 · 外设", 24)
stext(1300, 2090, "中断控制器 · DMA · 总线 · 时钟", 17)
sarrow(1300, 1974, 1300, 1986)

S.append("</svg>")
SVG_BODY = "\n".join(S)

svg_path = r"D:\GIT-SPACE\D00\_notes\linux_kernel_structure.svg"
with open(svg_path, "w", encoding="utf-8") as f:
    f.write(SVG_BODY)
print("svg:", svg_path)

# ============ HTML 查看器（图区支持滚轮缩放 + 拖拽平移） ============
CSS_ZOOM = """
  #stage { position: relative; overflow: hidden; border: 1px solid #e0e0e0; border-radius: 8px;
           background: #fff; cursor: grab; height: 74vh; }
  #stage.dragging { cursor: grabbing; }
  #view { transform-origin: 0 0; }
  #view svg { display: block; max-width: none; }
  .toolbar { display: flex; gap: 8px; margin-bottom: 10px; align-items: center; flex-wrap: wrap; }
  .toolbar button { padding: 4px 14px; border: 1px solid #ccc; background: #fafafa; border-radius: 6px;
                    cursor: pointer; font-size: 14px; }
  .toolbar button:hover { background: #eef3fb; }
  .toolbar .hint { color: #888; font-size: 12px; margin-left: auto; }
"""

JS_ZOOM = """
(function(){
  var stage = document.getElementById('stage');
  var view = document.getElementById('view');
  var SVG_W = 2600, SVG_H = 2160;
  var scale = 1, tx = 0, ty = 0;
  function apply(){ view.style.transform = 'translate(' + tx + 'px,' + ty + 'px) scale(' + scale + ')'; }
  function zoomAt(mx, my, f){
    var ns = Math.max(0.1, Math.min(8, scale * f));
    tx = mx - (mx - tx) * (ns / scale);
    ty = my - (my - ty) * (ns / scale);
    scale = ns; apply();
  }
  window.zoomBy = function(f){ var r = stage.getBoundingClientRect(); zoomAt(r.width/2, r.height/2, f); };
  window.setZoom = function(s){ var r = stage.getBoundingClientRect(); zoomAt(r.width/2, r.height/2, s/scale); };
  window.fit = function(){
    var r = stage.getBoundingClientRect();
    var s = Math.min(r.width / SVG_W, r.height / SVG_H, 1);
    scale = s; tx = (r.width - SVG_W * s) / 2; ty = (r.height - SVG_H * s) / 2; apply();
  };
  stage.addEventListener('wheel', function(e){
    e.preventDefault();
    var r = stage.getBoundingClientRect();
    zoomAt(e.clientX - r.left, e.clientY - r.top, e.deltaY < 0 ? 1.15 : 1/1.15);
  }, { passive: false });
  var drag = null;
  stage.addEventListener('mousedown', function(e){
    drag = { x: e.clientX, y: e.clientY, tx: tx, ty: ty };
    stage.classList.add('dragging'); e.preventDefault();
  });
  window.addEventListener('mousemove', function(e){
    if (!drag) return;
    tx = drag.tx + e.clientX - drag.x; ty = drag.ty + e.clientY - drag.y; apply();
  });
  window.addEventListener('mouseup', function(){ drag = null; stage.classList.remove('dragging'); });
  window.fit();
})();
"""

MMD = open(r"D:\GIT-SPACE\D00\_notes\linux_kernel_structure.mmd", encoding="utf-8").read()
html_doc = f"""<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<title>Linux 内核知识结构图（终极版）</title>
<style>
  body {{ font-family: "Microsoft YaHei", "PingFang SC", sans-serif; background: #f2f4f8; margin: 0; padding: 28px; color: #1f1f1f; }}
  h1 {{ text-align: center; color: #17365d; margin-bottom: 4px; }}
  .sub {{ text-align: center; color: #666; margin-bottom: 20px; font-size: 14px; }}
  .card {{ background: #fff; border-radius: 12px; box-shadow: 0 2px 12px rgba(0,0,0,.08); padding: 20px; margin: 0 auto 24px auto; max-width: 2640px; }}
  .card h2 {{ color: #17365d; font-size: 20px; margin: 0 0 12px 0; }}
  .mermaid {{ text-align: center; }}
  .mermaid svg {{ max-width: 100%; height: auto; }}
  footer {{ text-align: center; color: #999; font-size: 12px; margin-top: 8px; }}
{CSS_ZOOM}
</style>
</head>
<body>
<h1>🐧 Linux 内核知识结构图（终极版）</h1>
<div class="sub">图上滚轮缩放 · 按住左键拖拽平移 · 自底向上：硬件 → 体系结构 → 内核核心 → 系统调用 → 用户空间</div>

<div class="card">
  <h2>📊 矢量总览图（滚轮/按钮缩放，拖拽平移）</h2>
  <div class="toolbar">
    <button onclick="zoomBy(1.3)">＋ 放大</button>
    <button onclick="zoomBy(1/1.3)">－ 缩小</button>
    <button onclick="setZoom(1)">100%</button>
    <button onclick="fit()">适应窗口</button>
    <span class="hint">提示：在图上滚动滚轮缩放 · 按住左键拖拽平移</span>
  </div>
  <div id="stage">
    <div id="view">
{SVG_BODY}
    </div>
  </div>
</div>

<div class="card">
  <h2>🎨 Mermaid 实时渲染（联网自动加载）</h2>
  <pre class="mermaid">
{H.escape(MMD)}
  </pre>
</div>

<footer>生成于 VS Code 工作台 · 配套文档：linux_kernel_structure.md（源码目录地图 / 学习路径 / 工具链）</footer>
<script>{JS_ZOOM}</script>
<script src="https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.min.js"></script>
<script>mermaid.initialize({{ startOnLoad: true, theme: "dark" }});</script>
</body>
</html>
"""
html_path = r"D:\GIT-SPACE\D00\_notes\linux_kernel_structure.html"
with open(html_path, "w", encoding="utf-8") as f:
    f.write(html_doc)
print("html:", html_path)
