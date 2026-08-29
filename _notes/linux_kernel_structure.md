# Linux 内核知识结构图（顶级版）

> 一站式地图：总览图 → 源码目录地图 → 子系统速查 → 学习路径 → 工具链
> 在 VS Code 中按 `Ctrl+Shift+V` 预览渲染效果。

## 一、总览图

```mermaid
%%{init: {"theme": "dark", "themeVariables": {"background": "#1e1e1e", "primaryColor": "#333d4f", "primaryTextColor": "#e8e8e8", "primaryBorderColor": "#8a9bb8", "lineColor": "#9bb7d4", "fontFamily": "Microsoft YaHei", "clusterBkg": "#262b33", "clusterBorder": "#4d5a6e", "edgeLabelBackground": "#262b33", "nodeBorder": "#8a9bb8", "nodeTextColor": "#e8e8e8"}}}%%
flowchart TB
    classDef usr fill:#2471a3,stroke:#85c1e9,color:#fff
    classDef sci fill:#27ae60,stroke:#82e0aa,color:#fff
    classDef proc fill:#d35400,stroke:#ffa64d,color:#fff
    classDef mem fill:#b7950b,stroke:#f1c40f,color:#fff
    classDef fst fill:#1e8449,stroke:#58d68d,color:#fff
    classDef net fill:#1a5276,stroke:#5dade2,color:#fff
    classDef ipc fill:#6c3483,stroke:#af7ac5,color:#fff
    classDef irq fill:#c0392b,stroke:#f1948a,color:#fff
    classDef drv fill:#0e6655,stroke:#48c9b0,color:#fff
    classDef sec fill:#515a5a,stroke:#aeb6bf,color:#fff
    classDef xcut fill:#148f77,stroke:#76d7c4,color:#fff
    classDef arch fill:#7d6608,stroke:#f4d03f,color:#fff
    classDef hw fill:#4d5656,stroke:#99a3a4,color:#fff

    subgraph USR["① 用户空间 User Space"]
        APP["应用程序 / 服务<br/>Apache · MySQL · GUI"]
        LIBC["标准 C 库<br/>glibc / musl"]
        SH["Shell 与系统工具"]
        RT["运行时<br/>JVM / 语言运行时 / 容器编排"]
    end

    SCI["② 系统调用接口 SCI + vDSO<br/>open · read · write · fork · execve · mmap · ioctl · epoll · io_uring"]

    subgraph KERNEL["③ 内核核心 Kernel Core（Ring0 / EL1）"]
        subgraph PROC["进程与调度"]
            PM["进程管理<br/>task_struct · fork/exit · 线程 · namespace"]
            SCH["调度器<br/>EEVDF(CFS) · 实时/Deadline · 负载均衡 · PREEMPT_RT"]
        end
        subgraph MEM["内存管理"]
            MM1["虚拟内存<br/>页表 · MMU · 缺页 · COW"]
            MM2["物理内存<br/>伙伴系统 · slab/slub · CMA · HugePage"]
            MM3["页缓存与回收<br/>page cache · swap/zram · KSM · OOM"]
        end
        subgraph FST["文件系统与存储"]
            VFS["VFS 抽象<br/>dentry · inode · file · super_block"]
            FS2["具体文件系统<br/>ext4 · xfs · btrfs · proc/sysfs · tmpfs"]
            BIO["块层与 IO<br/>bio · I/O 调度 · io_uring · DMA"]
        end
        subgraph NET["网络协议栈"]
            N1["网络分层<br/>socket · TCP/UDP · IP · 邻居子系统"]
            N2["转发与过滤<br/>netfilter/iptables · TC · 路由"]
            N3["高速数据面<br/>NAPI · XDP · eBPF · DPDK"]
        end
        subgraph IPC_SYNC["并发与通信"]
            IPC["进程间通信<br/>管道 · 信号 · SysV · futex · socket"]
            SYNC["同步机制<br/>原子 · 自旋锁 · 互斥锁 · RCU · seqlock · 内存屏障"]
        end
        subgraph IRQ_T["中断与时间"]
            IRQ["中断子系统<br/>上半部/下半部 · softirq · workqueue · threaded IRQ"]
            TIME["时间管理<br/>jiffies · hrtimer · clockevent · NO_HZ"]
        end
        subgraph DRV_V["驱动与虚拟化"]
            DRV["设备驱动<br/>字符/块/网络 · 驱动模型 · 设备树/ACPI · DMA"]
            VM["虚拟化与容器<br/>KVM · virtio · vfio · cgroup v2 · namespace"]
        end
        subgraph SEC_P["安全与电源"]
            SEC["安全框架<br/>LSM · SELinux/AppArmor · lockdown · 密钥环"]
            PWR["电源管理<br/>cpuidle · cpufreq · runtime PM · suspend"]
        end
        subgraph XCUT["横切关注点"]
            INIT["内核初始化<br/>start_kernel → init"]
            DBG["调试与观测<br/>printk · ftrace · kprobe · perf · eBPF/BTF · kgdb · KUnit"]
            BLD["构建与开发<br/>Kconfig · Kbuild · LLVM/Clang · 编码规范"]
        end
    end

    subgraph ARCH["④ 体系结构层 arch/"]
        AR["x86 / ARM / RISC-V<br/>启动汇编 · 页表/MMU · APIC/GIC · 原子与屏障实现"]
    end

    HW["⑤ 硬件层<br/>CPU · 内存 · 磁盘 · 网卡 · 外设 · DMA · 总线"]

    APP --> LIBC
    LIBC --> SCI
    SH -.-> SCI
    RT -.-> SCI
    SCI --> KERNEL
    KERNEL --> AR
    AR --> HW
    XCUT -.-> PROC

    class APP,LIBC,SH,RT usr
    class SCI sci
    class PM,SCH proc
    class MM1,MM2,MM3 mem
    class VFS,FS2,BIO fst
    class N1,N2,N3 net
    class IPC,SYNC ipc
    class IRQ,TIME irq
    class DRV,VM drv
    class SEC,PWR sec
    class INIT,DBG,BLD xcut
    class AR arch
    class HW hw
```

## 二、内核源码目录地图（知识 → 代码）

| 知识领域 | 源码目录 | 关键文件 / 入口 |
| --- | --- | --- |
| 进程 / 调度 | `kernel/` | `kernel/fork.c` · `kernel/sched/core.c` · `kernel/sched/fair.c` |
| 内存管理 | `mm/` | `mm/mmap.c` · `mm/page_alloc.c` · `mm/slab.c` · `mm/vmscan.c` |
| 文件系统 | `fs/` | `fs/open.c` · `fs/read_write.c` · `fs/dcache.c` · `fs/ext4/` |
| 网络协议栈 | `net/` | `net/socket.c` · `net/ipv4/` · `net/core/` · `net/netfilter/` |
| 驱动模型 | `drivers/` + `include/linux/` | `drivers/base/` · `drivers/base/platform.c` |
| 中断 / 时间 | `kernel/irq/` + `kernel/time/` | `kernel/irq/chip.c` · `kernel/time/hrtimer.c` |
| IPC | `ipc/` + `kernel/` | `ipc/shm.c` · `ipc/msg.c` · `ipc/sem.c` · `kernel/futex.c` |
| 同步机制 | `kernel/locking/` + `include/linux/` | `include/linux/spinlock.h` · `kernel/locking/mutex.c` · `kernel/rcu/` |
| 虚拟化 / 容器 | `virt/` + `kernel/` | `virt/kvm/` · `drivers/virtio/` · `kernel/cgroup/` |
| 安全 | `security/` | `security/security.c` · `security/lsm/` |
| 体系结构 | `arch/` | `arch/x86/kernel/` · `arch/arm64/` · `arch/riscv/` |
| 通用头文件 | `include/` | `include/linux/` · `include/uapi/` |
| 工具 / 测试 | `tools/` | `tools/perf/` · `tools/testing/selftests/` |

## 三、子系统速查表

| 子系统 | 核心概念 | 必读入口 |
| --- | --- | --- |
| 调度 | task_struct · sched_class · EEVDF 虚拟运行时间 · runqueue | `kernel/sched/fair.c` |
| 内存 | 4 级页表 · 伙伴系统 order · slab 缓存 · LRU 回收 | `mm/page_alloc.c` |
| 文件 | path → dentry → inode → file · 页缓存 xarray | `fs/dcache.c` |
| 网络 | sk_buff · 协议分用 · softirq NET_RX | `net/core/skbuff.c` |
| 同步 | 原子(单指令) / 自旋(短临界区) / 互斥(可睡眠) / RCU(读多写少) | `Documentation/locking/` |
| 中断 | 上下半部拆分：顶半部快、底半部延后 | `kernel/irq/manage.c` |

## 四、学习路径

1. **地基**：C 语言 + 常用内核数据结构（链表 / 红黑树 / xarray / maple tree）
2. **进程与调度**：生命周期 → EEVDF 调度原理 → 线程与 namespace
3. **内存管理**：虚拟地址空间 → 页表 → 伙伴系统/slab → 缺页异常与回收
4. **同步机制**：原子/自旋/互斥/RCU 的适用场景（并发编程的地基）
5. **文件系统 + 块层**：VFS 抽象 → ext4 → 页缓存回写 → io_uring
6. **网络栈**：socket 层 → TCP/IP → netfilter → NAPI/XDP
7. **中断与时间**：上下半部 → workqueue → hrtimer → NO_HZ
8. **驱动开发**：驱动模型 → 设备树/ACPI → DMA → 中断
9. **观测利器**：ftrace / kprobe / perf / eBPF 深入内核行为

## 五、观测工具链

| 场景 | 工具 | 一句话用法 |
| --- | --- | --- |
| 函数调用追踪 | ftrace | `echo function > current_tracer` 后读 trace |
| 动态插桩 | kprobe / uprobe | 运行时探测任意函数入口/出口 |
| 内核级编程 | eBPF | bpftrace/BCC 写脚本观测系统调用、延迟、锁 |
| 性能剖析 | perf | `perf record -g` + `perf report` 出火焰图 |
| 崩溃分析 | kdump / crash | 内核 panic 后离线分析内存转储 |
| 回归测试 | KUnit / kselftest | 内核单元测试与自测套件 |
