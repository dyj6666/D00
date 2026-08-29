# -*- coding: utf-8 -*-
"""交互式知识结构图生成器 v4 —— 全量视图 + 精讲抽屉 + 资源链接
输出: linux_kernel_structure_interactive.html（离线可用）"""
import json
import html as H

FAM = "Microsoft YaHei, PingFang SC, sans-serif"

# ================= 内容数据 =================
# 一层子图：key -> (标题, [列])；列 = (列名, [(条目, 详情, 可选专题key)...])
SUBS = {
    "proc": ("进程与调度", [
        ("进程模型", [("task_struct", "进程描述符 · 内核栈 · 调度实体"), ("生命周期", "创建(fork) → 就绪 → 运行 → 阻塞 → 退出"), ("线程", "CLONE_THREAD · 共享地址空间"), ("命名空间", "PID/挂载/网络 namespace 隔离")]),
        ("调度核心", [("🔗 EEVDF", "虚拟运行时间 · 红黑树就绪队列", "deep_eevdf", None, "n_eevdf"), ("调度类", "stop → DL → RT → CFS/EEVDF → idle"), ("实时调度", "SCHED_FIFO/RR · 优先级继承"), ("Deadline", "SCHED_DEADLINE · GEDF"), ("负载均衡", "per-CPU runqueue · 推拉迁移"), ("组调度", "cgroup 带宽控制")]),
        ("切换与栈", [("上下文切换", "switch_to · 寄存器保存/恢复"), ("内核栈", "thread_info · 栈溢出防护"), ("调度入口", "tick · wakeup · 抢占点")]),
        ("源码入口", [("kernel/sched/", "core.c · fair.c · rt.c · deadline.c"), ("kernel/", "fork.c · exit.c · kthread.c")]),
    ]),
    "mem": ("内存管理", [
        ("虚拟地址空间", [("用户/内核空间", "TASK_SIZE 划分 · 4级页表"), ("直接映射", "线性映射区 · vmalloc 区"), ("mm_struct", "进程内存描述 · VMA 红黑树")]),
        ("物理内存", [("🔗 伙伴系统", "order 块 · per-CPU 页池 · 碎片", "deep_buddy", None, "n_buddy"), ("slab/slub", "对象缓存 · kmalloc"), ("CMA", "连续内存预留 · 迁移"), ("HugePage", "HugeTLB · THP 透明大页")]),
        ("映射与缺页", [("mmap", "文件/匿名映射 · 惰性分配"), ("缺页异常", "do_page_fault 路径"), ("COW", "写时复制 · fork 优化"), ("KSM", "同页合并")]),
        ("回收", [("LRU", "active/inactive 链表"), ("swap/zram", "交换分区 · 压缩内存"), ("OOM", "oom killer 选择"), ("cgroup 内存", "memory.max 限制")]),
        ("源码入口", [("mm/", "mmap.c · page_alloc.c · slab.c · vmscan.c"), ("include/linux/mm.h", "核心结构")]),
    ]),
    "net": ("网络协议栈", [
        ("socket 层", [("系统调用", "socket/bind/connect/send/recv"), ("sock 与 proto", "协议对象 · 接收队列"), ("缓冲区", "sk_buff · skb 池")]),
        ("传输层", [("🔗 TCP", "状态机 · 三次握手 · 四次挥手", "deep_tcp"), ("拥塞控制", "cubic/bbr · 窗口管理"), ("UDP", "无连接 · 校验和")]),
        ("网络层", [("IP", "分片 · TTL · 多路径"), ("路由", "fib 表 · 策略路由"), ("邻居子系统", "ARP/ND · 邻居缓存")]),
        ("收包路径", [("硬中断", "网卡中断 → NAPI 调度"), ("NAPI", "轮询收包 · 减少中断"), ("softirq", "NET_RX_SOFTIRQ 处理"), ("协议分用", "IP→TCP/UDP→socket")]),
        ("数据面", [("netfilter", "钩子链 · iptables/nftables"), ("TC", "队列规则 · 流量整形"), ("XDP/eBPF", "驱动层旁路 · 高速处理")]),
        ("源码入口", [("net/", "core/ · ipv4/ · ipv6/ · netfilter/ · sched/"), ("include/net/", "协议结构")]),
    ]),
    "fst": ("文件系统与存储", [
        ("VFS 抽象", [("🔗 路径解析", "dentry/inode/file · path walk · dcache", "deep_vfs", None, "n_vfs"), ("挂载树", "mount 实例 · 命名空间"), ("文件操作表", "fops/inode_ops 回调")]),
        ("页缓存", [("🔗 页缓存与回写", "address_space · 脏页 · flusher 线程", "deep_wb"), ("预读", "readahead 策略"), ("mmap 页缓存", "文件映射共用缓存")]),
        ("块层", [("bio", "块 IO 请求单元"), ("I/O 调度", "mq-deadline · BFQ"), ("io_uring", "异步 IO 提交/完成队列", None, None, "n_io_uring")]),
        ("具体文件系统", [("ext4", "jbd2 日志 · 兼容性之王"), ("xfs", "B+树 · 大文件扩展性"), ("btrfs", "写时复制 · 快照"), ("proc/sysfs", "内核信息导出 · 配置")]),
        ("源码入口", [("fs/", "dcache.c · read_write.c · buffer.c · ext4/"), ("block/", "bio.c · mq-deadline.c")]),
    ]),
    "ipc_sync": ("并发与通信", [
        ("原子与屏障", [("atomic_t", "单指令原子操作"), ("READ_ONCE/WRITE_ONCE", "防编译器重排"), ("内存屏障", "编译器屏障 vs CPU 屏障")]),
        ("锁原语", [("自旋锁", "短临界区 · 不可睡眠"), ("互斥锁", "可睡眠 · 优先级继承"), ("读写锁", "读多写少"), ("seqlock", "读无锁 · 写优先"), ("percpu 锁", "无竞争设计")]),
        ("RCU 专题", [("🔗 RCU", "读侧无锁 · 宽限期 · 延迟回收", "deep_rcu", None, "n_rcu")]),
        ("IPC 机制", [("管道/FIFO", "字节流 · 父子进程"), ("信号", "异步通知 · 处理函数"), ("System V", "shm/msg/sem 三件套"), ("futex", "用户态快速路径"), ("socketpair", "本地双向通信")]),
        ("锁决策", [("临界区多短?", "自旋/原子"), ("会不会睡眠?", "互斥/信号量"), ("读多写少?", "RCU/seqlock"), ("每个CPU独立?", "percpu")]),
        ("源码入口", [("kernel/locking/", "mutex.c · rwsem.c · spinlock"), ("kernel/rcu/", "tree.c · update.c"), ("ipc/", "shm.c · msg.c · sem.c")]),
    ]),
    "irq_t": ("中断与时间", [
        ("中断入口", [("中断控制器", "GIC(ARM) · APIC(x86)"), ("irq domain", "硬件中断号映射"), ("中断描述符", "irq_desc · 动作链表")]),
        ("下半部机制", [("🔗 下半部机制", "顶半部快 · softirq · workqueue · threaded IRQ", "deep_bh")]),
        ("时间体系", [("jiffies/tick", "节拍计数 · HZ"), ("clockevent", "可编程定时器"), ("clocksource", "高精度时间源"), ("hrtimer", "高精度定时器"), ("NO_HZ", "动态节拍 · tickless")]),
        ("源码入口", [("kernel/irq/", "handle.c · manage.c · chip.c"), ("kernel/time/", "timer.c · hrtimer.c · tick-*")]),
    ]),
    "drv_v": ("驱动与虚拟化", [
        ("驱动模型", [("bus/device/driver", "三层结构 · probe 流程"), ("platform 总线", "设备树驱动的默认总线"), ("设备树/ACPI", "硬件描述 · 匹配表")]),
        ("驱动类型", [("字符设备", "cdev · file_operations"), ("块设备", "gendisk · 请求处理"), ("网络设备", "net_device · ndo 回调")]),
        ("DMA", [("映射 API", "dma_map_single · SG"), ("一致性/流式", "两种映射语义"), ("IOMMU/SMMU", "设备地址隔离")]),
        ("KVM 专题", [("🔗 KVM 虚拟化", "vcpu · 陷入陷出 · 内存虚拟化", "deep_kvm")]),
        ("容器基石", [("cgroup v2", "资源控制 · 层级"), ("namespace", "隔离 · clone 标志"), ("seccomp", "系统调用过滤")]),
        ("源码入口", [("drivers/base/", "bus.c · driver.c · platform.c"), ("virt/kvm/", "kvm_main.c · kvm_mmu/")]),
    ]),
    "sec_p": ("安全与电源", [
        ("安全框架", [("LSM", "security hooks · 模块化"), ("SELinux/AppArmor", "强制访问控制两大派"), ("capabilities", "特权细分 · bounding set"), ("模块签名", "内核完整性 · lockdown"), ("密钥环", "keyring · 会话密钥")]),
        ("电源管理", [("cpuidle", "C 状态 · 空闲选择"), ("cpufreq", "DVFS · governor 策略"), ("runtime PM", "设备动态电源管理"), ("suspend/resume", "系统睡眠流程")]),
        ("源码入口", [("security/", "security.c · lsm/ · selinux/"), ("kernel/power/", "main.c · suspend.c"), ("drivers/cpufreq/", "governor 实现")]),
    ]),
    "xcut": ("横切关注点", [
        ("内核启动", [("🔗 内核启动", "BIOS/UEFI → bootloader → head → start_kernel", "deep_boot")]),
        ("调试与观测", [("printk", "日志级别 · dmesg"), ("ftrace", "函数追踪 · 延迟分析"), ("kprobe/uprobe", "动态插桩"), ("perf", "性能剖析 · 火焰图"), ("eBPF/BTF", "内核编程 · 可观测性", None, None, "n_bpf"), ("kgdb", "内核断点调试")]),
        ("构建与开发", [("Kconfig", "配置体系 · defconfig"), ("Kbuild", "Makefile 体系 · 模块编译"), ("LLVM/Clang", "新工具链 · LTO"), ("补丁流程", "LKML · git format-patch")]),
        ("源码入口", [("init/", "main.c · start_kernel"), ("kernel/", "printk/ · tracing/ · bpf/"), ("tools/", "perf/ · testing/")]),
    ]),
    "usr": ("用户空间", [
        ("应用视角", [("进程/线程", "用户态模型 · pthread"), ("信号处理", "handler · 信号屏蔽"), ("虚拟内存", "malloc 背后的 mmap/brk")]),
        ("标准 C 库", [("glibc/musl", "系统调用封装 · 性能差异"), ("动态链接", "ld.so · PLT/GOT"), ("线程库", "pthread · futex 实现")]),
        ("Shell 与工具", [("常用工具", "strace · ltrace · gdb · perf"), ("文本处理", "grep/awk/sed · 管道"), ("系统观测", "htop · pidstat · ss")]),
        ("ABI 与装载", [("ELF", "节区 · 符号表 · 重定位"), ("execve", "装载流程 · 解释器"), ("栈布局", "x86-64 ABI · 调用约定")]),
        ("学习建议", [("第一课", "strace 追踪一切系统调用"), ("必读", "APUE 前三章"), ("动手", "写一个 mini shell")]),
    ]),
    "sci": ("系统调用", [
        ("调用全流程", [("🔗 调用全流程", "陷入 → 查表 → 参数校验 → 执行 → 返回", "deep_syscall")]),
        ("机制要点", [("syscall table", "编号 · 架构差异"), ("参数传递", "寄存器约定 · 栈"), ("errno", "返回值与错误码")]),
        ("快速路径", [("vDSO", "gettimeofday 不进内核"), ("vsyscall", "遗留机制")]),
        ("常见族", [("进程", "fork/exec/wait/clone3"), ("文件", "open/read/write/stat"), ("内存", "mmap/brk/mprotect"), ("网络", "socket/connect/epoll"), ("事件", "epoll/eventfd/signalfd"), ("设备", "ioctl/read/write")]),
        ("现代接口", [("io_uring", "异步 · 免锁提交"), ("pidfd", "文件描述符管理进程"), ("openat2", "更安全的路径解析")]),
        ("学习建议", [("数一数", "x86-64 有多少个 syscall"), ("动手", "用 syscall() 绕过 glibc 写 read"), ("观察", "strace -f -e trace=file")]),
    ]),
    "arch": ("体系结构", [
        ("x86", [("启动", "real → protected → long mode"), ("中断", "APIC · IDT"), ("分页", "4 级页表 · PAE")]),
        ("ARM64", [("启动", "EL3 → EL2 → EL1"), ("中断", "GIC · 异常向量表"), ("分页", "4 级 · 16K/64K 页")]),
        ("RISC-V", [("启动", "M 态 → S 态"), ("中断", "PLIC · CLINT"), ("分页", "Sv39/Sv48")]),
        ("共性对比", [("原子操作", "x86 lock · ARM LSE · RISC-V amo"), ("屏障指令", "mfence · dsb · fence"), ("内存模型", "TSO vs 弱内存序")]),
        ("源码入口", [("arch/x86/", "kernel/head_64.S · entry/"), ("arch/arm64/", "kernel/head.S · kvm/"), ("arch/riscv/", "kernel/head.S")]),
    ]),
    "hw": ("硬件层", [
        ("CPU", [("核心/线程", "超线程 · 大小核"), ("缓存层级", "L1/L2/L3 · 一致性协议"), ("执行模型", "乱序 · 分支预测 · 流水线")]),
        ("内存系统", [("DDR", "通道 · 带宽 · 延迟"), ("NUMA", "本地/远端内存访问"), ("存储层级", "寄存器→缓存→内存→磁盘")]),
        ("总线与外设", [("PCIe", "拓扑 · BAR · MSI-X"), ("USB/SPI/I2C", "低速外设总线"), ("DMA 控制器", "搬运数据 · 中断通知"), ("中断控制器", "GIC/APIC · 亲和性")]),
        ("观测工具", [("lscpu", "CPU 拓扑"), ("lstopo", "NUMA 拓扑图"), ("lspci", "PCIe 设备树")]),
    ]),
}

# 二层专题：key -> (标题, [列])
DEEP = {
    "deep_eevdf": ("EEVDF 调度算法专题", [
        ("核心思想", [("虚拟运行时间", "ve = 实际运行时间 / 权重"), ("红黑树", "按 ve 排序 · 取最左节点"), ("延迟公平", "ve 最小者优先 · 抢占")]),
        ("与 CFS 对比", [("CFS(旧)", "按 vruntime 最小优先"), ("EEVDF(6.6+)", "带延迟界限的公平调度"), ("淘汰原因", "唤醒延迟不公 · 交互性差")]),
        ("关键概念", [("权重", "nice 值 → 权重映射"), ("slice", "调度周期 · 时间片"), ("lag", "滞后补偿机制")]),
        ("源码入口", [("kernel/sched/fair.c", "pick_eevdf · update_curr"), ("Documentation/", "scheduler/sched-design-CFS.rst")]),
    ]),
    "deep_rcu": ("RCU 同步机制专题", [
        ("为什么快", [("读侧", "无锁 · 无原子操作 · 无内存屏障"), ("写侧", "复制替换 · 等待旧读者退出")]),
        ("核心概念", [("grace period", "宽限期 · 所有读者退出"), ("quiescent state", "静止点 · 读者经过点"), ("延迟回收", "callback 机制 · 内存安全")]),
        ("变体", [("rcu_read_lock", "禁止抢占模式"), ("synchronize_rcu", "同步等待宽限期"), ("call_rcu", "异步回调"), ("srcu", "睡眠读者"), ("rcu_barrier", "等待全部回调")]),
        ("使用场景", [("链表替换", "读写并发 · 读多写少"), ("内核经典", "路由表 · 文件系统 dcache"), ("禁忌", "读侧不能睡眠(经典RCU)")]),
        ("源码入口", [("kernel/rcu/", "tree.c · update.c · sync.c"), ("include/linux/rcupdate.h", "读侧宏")]),
    ]),
    "deep_wb": ("页缓存与回写专题", [
        ("页缓存结构", [("address_space", "文件页 → 页缓存映射"), ("radix/xarray", "页索引 → 页描述符"), ("文件读写路径", "page cache 命中/未命中")]),
        ("脏页生命周期", [("标记脏", "写入即脏 · 回写候选"), ("flusher 线程", "周期/阈值/同步三种触发"), ("balance_dirty_pages", "写回压力反馈")]),
        ("持久化保证", [("fsync/fdatasync", "刷盘 · 元数据"), ("日志文件系统", "jbd2 先记日志再落盘"), ("掉电安全", "journal 重放")]),
        ("源码入口", [("mm/filemap.c", "page cache 核心"), ("mm/page-writeback.c", "回写控制"), ("fs/fs-writeback.c", "wb 工作队列")]),
    ]),
    "deep_vfs": ("VFS 路径解析专题", [
        ("四个对象", [("super_block", "文件系统实例"), ("inode", "文件元数据"), ("dentry", "目录项 · 路径组件"), ("file", "打开实例 · 偏移")]),
        ("路径解析", [("path walk", "逐分量查找 · dcache 命中"), ("RCU-walk", "无锁快速路径"), ("挂载点穿越", "mount 栈 · 切换根")]),
        ("性能技巧", [("dcache 缓存", "热点路径命中率极高"), ("负缓存", "不存在路径也缓存"), ("符号链接", "follow_link · 深度限制")]),
        ("源码入口", [("fs/namei.c", "path_walk · link_path_walk"), ("fs/dcache.c", "d_lookup · dput")]),
    ]),
    "deep_bh": ("中断下半部机制专题", [
        ("为什么分两半", [("硬中断要求", "快 · 不可阻塞 · 关中断"), ("延迟工作", "可阻塞 · 可睡眠处理")]),
        ("机制谱系", [("softirq", "固定类型 · 高吞吐"), ("tasklet", "softirq 封装 · 串行"), ("workqueue", "进程上下文 · 可睡眠"), ("threaded IRQ", "每个中断一个内核线程")]),
        ("选择决策", [("吞吐优先", "softirq（网络收包）"), ("简单串行", "tasklet"), ("可睡眠/耗时", "workqueue"), ("实时性", "threaded IRQ + 优先级")]),
        ("网络场景", [("NAPI", "中断触发轮询 · softirq 处理"), ("NET_RX_SOFTIRQ", "收包主战场")]),
        ("源码入口", [("kernel/softirq.c", "__do_softirq"), ("kernel/workqueue.c", "worker 线程"), ("kernel/irq/manage.c", "request_threaded_irq")]),
    ]),
    "deep_buddy": ("伙伴系统专题", [
        ("核心思想", [("order", "2^order 页块 · 11 级链表"), ("分配", "最佳适应 · 大块分裂"), ("释放", "伙伴合并 · 页迁移")]),
        ("性能设计", [("per-CPU 页池", "无锁热页分配"), ("冷热页", "cache 亲和性"), ("migratetype", "按迁移类型分组防碎片")]),
        ("碎片问题", [("外部碎片", "反碎片 · 迁移"), ("CMA", "预留可迁移区"), ("compact", "内存规整")]),
        ("源码入口", [("mm/page_alloc.c", "alloc_pages · __free_pages"), ("mm/compaction.c", "规整路径")]),
    ]),
    "deep_tcp": ("TCP 拥塞控制专题", [
        ("核心问题", [("窗口", "发送/接收窗口 · 流量控制"), ("拥塞窗口", "cwnd · 网络容量探测"), ("慢启动", "指数增长 · 到阈值转线性")]),
        ("算法演进", [("Reno", "经典 AIMD"), ("Cubic", "Linux 默认 · 高带宽长肥管道"), ("BBR", "基于带宽时延积 · 不丢包探测")]),
        ("关键机制", [("重传", "RTO · 快速重传 · SACK"), ("乱序容忍", "dup-ack 计数"), ("ECN", "显式拥塞通知")]),
        ("源码入口", [("net/ipv4/tcp_cong.c", "拥塞控制框架"), ("net/ipv4/tcp_cubic.c", "Cubic 实现"), ("net/ipv4/tcp_output.c", "发送路径")]),
    ]),
    "deep_kvm": ("KVM 虚拟化专题", [
        ("架构", [("vCPU", "线程模型 · 每 vCPU 一任务"), ("kvm_run", "退出原因 · 用户态处理"), ("VMX/SVM", "硬件虚拟化指令")]),
        ("运行模型", [("陷入陷出", "VM exit 原因分类"), ("直通设备", "vfio · 中断重映射"), ("virtio", "前后端共享队列")]),
        ("内存虚拟化", [("EPT/NPT", "二级页表 · 免影子页表"), ("脏页追踪", "迁移 · 快照"), ("大页映射", "HugeTLB 直通")]),
        ("源码入口", [("virt/kvm/", "kvm_main.c · kvm_mmu/"), ("arch/x86/kvm/", "vmx.c · svm.c · mmu.c")]),
    ]),
    "deep_syscall": ("系统调用全流程专题", [
        ("用户态入口", [("glibc 封装", "syscall() · 内联汇编"), ("vDSO 例外", "免陷入的快速路径")]),
        ("陷入内核", [("syscall 指令", "x86-64 · MSR 指定入口"), ("入口汇编", "entry_SYSCALL_64 · pt_regs")]),
        ("查表执行", [("sys_call_table", "编号 → 函数指针"), ("参数校验", "copy_from_user · 范围检查"), ("权限检查", "capable() · LSM 钩子")]),
        ("返回路径", [("返回值", "rax · 负值表错误"), ("errno 设置", "用户态 -retval"), ("信号/抢占点", "返回前检查")]),
        ("源码入口", [("arch/x86/entry/", "entry_64.S · common.c"), ("include/linux/syscalls.h", "声明宏")]),
    ]),
    "deep_boot": ("内核启动流程专题", [
        ("固件阶段", [("BIOS/UEFI", "硬件初始化 · 引导选择"), ("Bootloader", "GRUB/systemd-boot · 加载内核")]),
        ("汇编阶段", [("head_64.S", "页表建立 · 进入 C 环境"), ("解压", "decompress_kernel")]),
        ("C 阶段", [("start_kernel", "初始化一切 · 第一个 C 函数"), ("关键子步骤", "trap_init · mm_init · sched_init"), ("init 进程", "kernel_init → 用户态 PID1")]),
        ("观测方法", [("earlyprintk", "最早的日志输出"), ("initcall_debug", "启动阶段跟踪"), ("bootgraph", "启动耗时分析")]),
        ("源码入口", [("init/main.c", "start_kernel 全流程"), ("arch/x86/kernel/head_64.S", "汇编入口")]),
    ]),
}
DEEP_PARENT = {"deep_eevdf": "proc", "deep_rcu": "ipc_sync", "deep_wb": "fst", "deep_vfs": "fst",
               "deep_bh": "irq_t", "deep_buddy": "mem", "deep_tcp": "net", "deep_kvm": "drv_v",
               "deep_syscall": "sci", "deep_boot": "xcut"}

# 阶段 0 · 内核 C 地基：key -> (标题, [列])；条目 (名称, 详情) | (名称, 详情, 源码路径) | (名称, 详情, 源码路径, 代码片段)
FOUND = {
    "gcc_ext": ("GNU C 扩展（内核专用语法）", [
        ("属性 attribute", [("__attribute__((packed))", "结构体紧凑布局 · 网络/磁盘结构必用", "include/linux/compiler_types.h", "__attribute__((packed))"),
                            ("__attribute__((aligned))", "缓存行对齐 · 防伪共享", None, "__attribute__((aligned(64)))"),
                            ("__attribute__((section))", "自定义段 · initcall 机制的基础", None, "__attribute__((section(\".init.text\")))"),
                            ("noreturn/format/unused", "编译器契约 · printf 风格检查")]),
        ("类型魔法", [("container_of", "内核第一宏 · 从成员反推结构体", "include/linux/container_of.h", "container_of(ptr, type, member)", "fdeep_container_of", "n_container_of"),
                      ("typeof", "类型推导 · 泛型宏基础", None, "typeof(x) tmp = x;"),
                      ("offsetof", "成员偏移 · 内核手写实现")]),
        ("语句表达式", [("({ ... })", "带返回值的语句块 · 宏安全", None, "#define MAX(a,b) ({ typeof(a) _a=(a); _b=(b); _a>_b?_a:_b; })"),
                        ("变长宏 ##__VA_ARGS__", "逗号剥离技巧"), ("do{}while(0)", "宏封装惯例 · 悬挂 else 防护")]),
        ("内建函数", [("__builtin_expect", "分支预测 · unlikely 宏的本质", "include/linux/compiler.h", "if (unlikely(err)) goto out;"),
                      ("__builtin_constant_p", "编译期常量判断 · 双路径宏"), ("__builtin_return_address", "调用者回溯")]),
        ("内联汇编", [("asm volatile", "屏障/原子指令的底层", None, "asm volatile(\"\" ::: \"memory\");"),
                      ("约束符", "r/m/i · 寄存器操作数"), ("实例", "arch 层原子操作实现")]),
        ("源码入口", [("include/linux/compiler_types.h", "编译魔法总汇", "include/linux/compiler_types.h"),
                      ("include/linux/container_of.h", "container_of 实现", "include/linux/container_of.h")]),
    ]),
    "memory_c": ("指针与内存模型（内核视角）", [
        ("指针进阶", [("多级指针", "**pp · 链表删除场景"), ("函数指针", "回调 · file_operations 的本质", "include/linux/fs.h", "struct file_operations *fops;"),
                      ("const 限定", "指针常量 vs 常量指针 · API 签名"), ("restrict", "别名优化提示")]),
        ("内存布局", [("段布局", "text/rodata/data/bss · 内核镜像"), ("内核栈", "8/16KB 固定大小 · 栈溢出即崩溃"),
                      ("大小端", "网络序 · cpu_to_le32 家族", None, "cpu_to_le32(value)"),
                      ("对齐与填充", "struct 布局 · padding · packed 取舍")]),
        ("位操作", [("原子位操作", "set_bit/clear_bit/test_bit", "arch/x86/include/asm/bitops.h", "set_bit(5, &flags)"),
                    ("位域", "内核基本不用 · 用位操作替代"), ("位图 API", "DECLARE_BITMAP · bitmap_* 家族", "include/linux/bitmap.h")]),
        ("源码入口", [("arch/x86/include/asm/bitops.h", "原子位操作实现", "arch/x86/include/asm/bitops.h"),
                      ("include/linux/bitmap.h", "位图 API", "include/linux/bitmap.h")]),
    ]),
    "ds": ("内核数据结构（必备八件套）", [
        ("链表", [("list_head", "双向循环 · 嵌入对象而非指针容器", "include/linux/list.h", "struct list_head list;", "fdeep_list", "n_list"),
                  ("遍历宏", "list_for_each_entry 全家族", "include/linux/list.h", "list_for_each_entry(p, &head, member)"),
                  ("hlist", "哈希链 · 头节点省一半内存"), ("增删改查", "list_add/del/replace · 复杂度 O(1)")]),
        ("树与映射", [("rbtree", "红黑树 · 调度/VMA/定时器", "lib/rbtree.c", "rb_insert_color(root)", None, "n_rbtree"),
                      ("xarray", "稀疏数组 · 页缓存索引", "lib/xarray.c", "xa_store(&xa, idx, ptr, GFP_KERNEL)", None, "n_xarray"),
                      ("maple tree", "6.1+ 新数据结构 · 取代部分 rbtree", "lib/maple_tree.c")]),
        ("队列与 ID", [("kfifo", "无锁环形缓冲 · 单生产单消费", "include/linux/kfifo.h", "kfifo_in(&fifo, buf, len)", None, "n_kfifo"),
                       ("idr/ida", "整数 ID 分配器 · 设备号管理", "lib/idr.c", "idr_alloc(&idr, ptr, 0, 0, GFP_KERNEL)")]),
        ("使用原则", [("嵌入 vs 指针", "内核容器全部内嵌进对象"), ("生命周期", "分配→初始化→使用→释放"), ("RCU 变体", "hlist_nulls · 无锁读者")]),
        ("源码入口", [("include/linux/list.h", "链表全部 API", "include/linux/list.h"),
                      ("lib/rbtree.c", "红黑树实现", "lib/rbtree.c"),
                      ("lib/xarray.c", "xarray 实现", "lib/xarray.c")]),
    ]),
    "api": ("API 惯例与内存分配", [
        ("错误处理", [("负数错误码", "-ENOMEM/-EINVAL 惯例 · 0 为成功"), ("ERR_PTR/IS_ERR", "指针与错误同路返回", "include/linux/err.h", "return ERR_PTR(-ENOMEM);"),
                      ("PTR_ERR/ERR_CAST", "错误码提取与类型转换")]),
        ("内存分配", [("kmalloc/kzalloc/kcalloc", "物理连续 · 小对象", "include/linux/slab.h", "ptr = kzalloc(size, GFP_KERNEL);"),
                      ("GFP 标志", "KERNEL/ATOMIC/NOWAIT 语义 · 睡眠与否", "include/linux/gfp_types.h", None, "fdeep_gfp", "n_gfp"),
                      ("vmalloc", "虚拟连续 · 大块 · 页面分散", "mm/vmalloc.c"), ("kmem_cache", "专用对象缓存 · slab 精确控制"),
                      ("失败检查", "分配必须查 NULL · 优雅降级", None, "if (!ptr) return -ENOMEM;")]),
        ("引用计数", [("kref", "get/put 生命周期管理", "include/linux/kref.h", "kref_get(&obj->ref); kref_put(&obj->ref, release);", None, "n_kref"),
                      ("对称性", "谁拿谁还 · 配对使用")]),
        ("日志与输出", [("printk 级别", "KERN_ERR/INFO/DEBUG · 控制台过滤", "kernel/printk/printk.c", "pr_info(\"init done\\n\");", None, "n_printk"),
                        ("格式扩展", "%pK 指针脱敏 · %pa 物理地址 · %pS 符号"), ("动态调试", "dynamic_debug 运行时开关")]),
        ("字符串安全", [("strscpy", "限长拷贝 · 替代 strcpy/strncpy", "include/linux/string.h", "strscpy(dst, src, sizeof(dst));"),
                        ("strnlen/strscpy_pad", "安全长度计算"), ("memcpy vs memmove", "重叠区域 · memmove 安全"),
                        ("FORTIFY 联动", "字符串函数编译期越界检测")]),
        ("命名与风格", [("函数命名", "xxx_alloc/xxx_free/xxx_init 惯例"), ("锁后缀", "_locked/_unlocked 约定"), ("返回值", "0 成功 / 负错误码")]),
        ("源码入口", [("include/linux/slab.h", "kmalloc 家族", "include/linux/slab.h"),
                      ("kernel/printk/printk.c", "printk 实现", "kernel/printk/printk.c")]),
    ]),
    "sync_base": ("并发地基（入门必备）", [
        ("原子操作", [("atomic_t", "单指令原子 · 计数器", "include/linux/atomic.h", "atomic_inc(&refcnt);"),
                      ("原子位", "set_bit/clear_bit · 标志位"), ("atomic_long_t", "长整型原子")]),
        ("访问纪律", [("READ_ONCE/WRITE_ONCE", "防撕裂读 · 防编译器优化", "include/linux/compiler.h", "val = READ_ONCE(x);"),
                      ("内存屏障", "编译器屏障 vs CPU 屏障 · smp_mb", None, None, "fdeep_barrier", "n_barrier"), ("数据竞争", "KCSAN 运行时检测")]),
        ("锁入门", [("自旋锁", "短临界区 · 不可睡眠 · 忙等", "include/linux/spinlock.h", "spin_lock(&lock); ... spin_unlock(&lock);", None, "n_spinlock"),
                    ("互斥锁", "可睡眠 · 内核默认选择", "kernel/locking/mutex.c", "mutex_lock(&m); ... mutex_unlock(&m);", None, "n_mutex"),
                    ("选择依据", "临界区时长 · 是否可能睡眠")]),
        ("无锁模式", [("kfifo 单生产者", "天然无锁环形"), ("seqlock", "读多写少 · 写者优先")]),
        ("源码入口", [("include/linux/spinlock.h", "自旋锁 API", "include/linux/spinlock.h"),
                      ("kernel/locking/mutex.c", "互斥锁实现", "kernel/locking/mutex.c")]),
    ]),
    "debug": ("内核调试与断言（地基级）", [
        ("断言与告警", [("WARN_ON/BUG_ON", "不可达路径检查 · 触发即崩溃", "include/asm-generic/bug.h", "WARN_ON_ONCE(!ptr);", None, "n_warn"),
                        ("dump_stack", "打印调用栈 · 现场取证"), ("panic", "致命错误 · 系统停止")]),
        ("printk 调试", [("pr_* 宏族", "pr_info/pr_err/pr_debug 分级", None, "pr_err(\"failed: %d\\n\", ret);"),
                         ("调试点选择", "错误路径 vs 热路径 · 别刷屏"), ("动态调试", "echo 'file xxx.c +p' > dynamic_debug")]),
        ("工具链", [("lockdep", "锁序检测 · 死锁预警", "kernel/locking/lockdep.c", None, None, "n_lockdep"), ("KASAN", "越界/UAF 检测 · 编译期开启", "mm/kasan/", None, None, "n_kasan"),
                    ("KCSAN", "数据竞争检测"), ("UBSAN", "未定义行为检测")]),
        ("Oops 解读", [("崩溃输出", "寄存器/栈/调用链三段式"), ("解码", "scripts/decode_stacktrace.sh", "scripts/decode_stacktrace.sh"),
                       ("常规手段", "addr2line · gdb vmlinux")]),
        ("源码入口", [("include/asm-generic/bug.h", "WARN/BUG 定义", "include/asm-generic/bug.h"),
                      ("scripts/decode_stacktrace.sh", "崩溃解码脚本", "scripts/decode_stacktrace.sh")]),
    ]),
    "module_dev": ("内核模块开发（第一个实践）", [
        ("模块骨架", [("module_init/module_exit", "入口/出口宏", "include/linux/module.h", "module_init(hello_init);", None, "n_module"),
                      ("MODULE_LICENSE", "GPL 声明 · EXPORT_SYMBOL 前提", None, "MODULE_LICENSE(\"GPL\");"),
                      ("元信息", "MODULE_AUTHOR/DESCRIPTION/ALIAS")]),
        ("编译体系", [("Kbuild Makefile", "obj-m += hello.o 两行搞定", None, "obj-m += hello.o"),
                      ("Kconfig", "配置项定义 · tristate"), ("符号导出", "EXPORT_SYMBOL / EXPORT_SYMBOL_GPL", None, "EXPORT_SYMBOL_GPL(my_fn);")]),
        ("装载与验证", [("insmod/rmmod/lsmod", "装载三连"), ("dmesg", "验证输出 · 成功标志"), ("modprobe", "依赖解析与参数")]),
        ("与内核交互", [("file_operations", "字符设备接口", "include/linux/fs.h", ".read = my_read, .write = my_write,"),
                        ("proc/sysfs", "kobject · 属性文件导出", "samples/kobject/kobject-example.c"),
                        ("module_param", "装载参数传递", None, "module_param(debug, int, 0644);")]),
        ("源码入口", [("samples/kobject/kobject-example.c", "官方示例", "samples/kobject/kobject-example.c"),
                      ("Documentation/kbuild/", "构建体系文档", "Documentation/kbuild/")]),
    ]),
    "c_deep": ("C 语言深水区（内核陷阱）", [
        ("类型与转换", [("整数提升", "小于 int 的类型参与运算先提升"), ("符号陷阱", "有符号/无符号比较 · -1 < 0u 为假", None, "if (len < 0 || len > SIZE_MAX)"),
                        ("char 符号性", "x86 有符号 · ARM 无符号 · 用 signed char/u8 显式声明"),
                        ("隐式转换", "方向与精度丢失 · 警告即错误")]),
        ("结构体与联合", [("柔性数组", "变长尾部 · 内核大量使用", "include/linux/stddef.h", "struct foo { int len; u8 data[]; };"),
                          ("union 类型双关", "网络栈 reinterpret 技巧 · 严格别名风险"),
                          ("匿名结构体/联合", "嵌套成员直接访问"), ("位域", "内核基本不用 · 用位操作")]),
        ("指针陷阱", [("数组退化", "数组形参即指针 · sizeof 失效"), ("悬垂指针", "释放后使用 · UAF 心智模型", None, "kfree(p); p = NULL;  // 防双释放"),
                      ("指针算术越界", "UB · UBSAN 可检测"), ("空指针解引用", "内核里是 BUG · 必须判空")]),
        ("控制流模式", [("goto err 清理", "内核错误处理第一模式", "kernel/fork.c", "if (ret) goto err_free;"),
                        ("资源配对", "获取与释放对称 · 锁/内存/引用"),
                        ("提前返回纪律", "错误路径不嵌套 · 扁平化")]),
        ("UB 清单", [("有符号溢出", "INT_MAX+1 · 内核用 __s64 谨慎"), ("除零/越界/未初始化", "UBSAN/KASAN 检测目标"),
                     ("严格别名违反", "不同型指针互访 · 用 union 或 memcpy")]),
        ("源码入口", [("Documentation/process/coding-style.rst", "内核编码风格圣经", "Documentation/process/coding-style.rst"),
                      ("include/linux/stddef.h", "flex array 相关定义", "include/linux/stddef.h")]),
    ]),
    "macro": ("预处理器与宏工程", [
        ("运算符", [("# 字符串化", "参数转字符串 · 调试宏", None, "#define S(x) #x"),
                    ("## 连接", "标识符拼接 · 表驱动生成", None, "#define MK(n) foo_##n"),
                    ("__VA_ARGS__", "变长宏参数 · ## 剥离逗号")]),
        ("编译期断言", [("BUILD_BUG_ON", "编译期检查 · 失败即编译错误", "include/linux/build_bug.h", "BUILD_BUG_ON(sizeof(x) != 8);"),
                        ("BUILD_BUG_ON_ZERO", "类型检查技巧")]),
        ("内核常用宏", [("min/max 与 min_t/max_t", "类型安全版本", "include/linux/minmax.h", "min_t(u32, a, b)"),
                        ("ARRAY_SIZE", "数组长度 · 越界防护", "include/linux/kernel.h", "for (i = 0; i < ARRAY_SIZE(tbl); i++)"),
                        ("FIELD_GET/FIELD_PREP", "位域读写宏", "include/linux/bitfield.h", "FIELD_GET(STATUS_MASK, reg)"),
                        ("round_up/DIV_ROUND_UP", "对齐与整除上取整"), ("IS_ALIGNED", "对齐判断")]),
        ("宏陷阱", [("参数副作用", "多求值问题 · 用语句表达式", None, "#define SQUARE(x) ((x)*(x))  // 慎用"),
                    ("括号包裹", "参数与整体必须全括号"), ("宏 vs 函数", "类型不检查 · 优先 inline 函数")]),
        ("头文件纪律", [("include 守卫", "#ifndef _XXX_H"), ("依赖最小化", "前向声明 · 避免循环包含"),
                        ("导出符号", "哪些宏进 UAPI")]),
        ("源码入口", [("include/linux/kernel.h", "ARRAY_SIZE · min/max 家族", "include/linux/kernel.h"),
                      ("include/linux/build_bug.h", "编译期断言", "include/linux/build_bug.h"),
                      ("include/linux/bitfield.h", "位域宏", "include/linux/bitfield.h")]),
    ]),
    "link_mem": ("编译链接与内存纵深", [
        ("编译流程", [("四阶段", "预处理→编译→汇编→链接", None, "gcc -E / -S / -c / ld"),
                      ("编译单元", "每个 .c 独立编译 · 头文件展开"), ("链接产物", "vmlinux · .ko · System.map")]),
        ("符号与链接", [("内部/外部链接", "static 限制作用域 · 符号表"), ("符号解析", "未定义引用 · 重复定义错误"),
                        ("重定位", "地址修正 · 位置无关代码")]),
        ("链接脚本", [("vmlinux.lds.S", "内核布局总纲", "arch/x86/kernel/vmlinux.lds.S", ".rodata : { *(.rodata) }", "fdeep_link"),
                      ("段顺序", "text→rodata→data→bss · 权限页对齐")]),
        ("优化与加固", [("-O2 语义", "内联 · 常量折叠 · 未定义行为利用", "scripts/Makefile.build"),
                        ("FORTIFY_SOURCE", "字符串函数越界编译期检测", None, "CONFIG_FORTIFY_SOURCE=y"),
                        ("stack protector", "栈金丝雀 · 防溢出"), ("LTO", "跨编译单元优化")]),
        ("ABI 与调用", [("栈帧布局", "参数/返回地址/局部变量 · 帧指针"), ("调用约定", "x86-64 SysV · ARM64 AAPCS"),
                        ("栈溢出原理", "返回地址覆写 · 金丝雀防护")]),
        ("缓存与内存序", [("缓存行", "64B · 伪共享", None, "____cacheline_aligned"),
                          ("局部性", "时间/空间局部性 · 内核数据结构布局"),
                          ("内存序入门", "TSO(x86) vs 弱序(ARM) · 屏障为什么存在")]),
        ("源码入口", [("arch/x86/kernel/vmlinux.lds.S", "链接脚本", "arch/x86/kernel/vmlinux.lds.S"),
                      ("scripts/Makefile.build", "编译规则", "scripts/Makefile.build")]),
    ]),
    "ds_adv": ("数据结构进阶（内核实战）", [
        ("哈希表", [("hlist", "哈希链 · 头节点单指针", "include/linux/list.h"),
                    ("rhashtable", "动态扩容 · 无锁读 · 网络栈标配", "lib/rhashtable.c", "rhashtable_init(&ht, &params);", None, "n_rhashtable"),
                    ("哈希函数选择", "jhash · 抗碰撞 · 随机种子")]),
        ("无锁链表", [("llist", "lockless singly list · 单消费者", "include/linux/llist.h", "llist_add(&node->llist, &head);"),
                      ("per-cpu 思想", "每 CPU 一份 · 免锁 · 最终合并")]),
        ("同步原语型结构", [("waitqueue", "等待队列 · 睡眠唤醒机制", "include/linux/wait.h", "wait_event(wq, condition);"),
                             ("completion", "完成量 · 一对一线程同步", "include/linux/completion.h", "wait_for_completion(&done);")]),
        ("对象管理", [("mempool", "预分配池 · 紧急分配保障", "mm/mempool.c", "mempool_alloc(pool, GFP_KERNEL);"),
                      ("kmem_cache", "对象缓存 · slab 精确控制"), ("idr 回顾", "整数 ID · 设备号")]),
        ("演进脉络", [("radix tree → xarray", "历史继承 · 稀疏索引"), ("rbtree 场景", "调度/vma/定时器"),
                      ("选择决策", "遍历多→链表 · 查找多→树/哈希 · 序重要→红黑树")]),
        ("源码入口", [("lib/rhashtable.c", "rhashtable 实现", "lib/rhashtable.c"),
                      ("include/linux/llist.h", "无锁链表", "include/linux/llist.h"),
                      ("mm/mempool.c", "mempool 实现", "mm/mempool.c")]),
    ]),
}

# 地基专题深挖：key -> (标题, [列])
FDEEP = {
    "fdeep_container_of": ("container_of 原理与嵌入设计", [
        ("实现解剖", [("宏定义", "offsetof + 类型检查 + 指针算术", "include/linux/container_of.h", "container_of(ptr, type, member)"),
                      ("offsetof", "编译期常量 · 零运行时开销"), ("指针算术", "(type *)0 技巧 · 减偏移")]),
        ("类型检查", [("__same_type 校验", "编译期类型匹配 · 用错即报错"), ("为什么安全", "编译器验证 + 偏移正确性")]),
        ("嵌入设计哲学", [("内核容器", "结构体嵌 list_head · 反推宿主"), ("对比指针容器", "嵌入省一次解引用 · 缓存友好"),
                          ("典型场景", "list_for_each_entry 本质就是 container_of")]),
        ("常见错误", [("传错成员名", "编译期拦截 · 类型检查兜底"), ("类型不符", "警告即错误")]),
        ("源码入口", [("include/linux/container_of.h", "实现全文", "include/linux/container_of.h")]),
    ]),
    "fdeep_list": ("list_head 实战手册", [
        ("初始化", [("静态/动态", "LIST_HEAD(name) · INIT_LIST_HEAD", None, "LIST_HEAD(mylist);"),
                    ("头尾哨兵", "空链表自指 · 永不 NULL")]),
        ("增删改查", [("list_add/add_tail", "头插/尾插 · O(1)"), ("list_del", "摘除 · 不释放内存"),
                      ("list_entry", "从节点反推对象")]),
        ("安全遍历", [("list_for_each_entry_safe", "遍历中删除必须 safe 版", "include/linux/list.h", "list_for_each_entry_safe(p, n, &head, member)"),
                      ("为什么需要 safe", "删除会破坏 next 指针")]),
        ("排序与合并", [("list_sort", "内核归并排序", "lib/list_sort.c"), ("list_splice", "链表拼接")]),
        ("实战：LRU", [("实现思路", "头插新项 · 尾删最旧"), ("命中提升", "移动到头 · 双向链表优势")]),
        ("源码入口", [("include/linux/list.h", "全部 API", "include/linux/list.h"),
                      ("lib/list_sort.c", "排序实现", "lib/list_sort.c")]),
    ]),
    "fdeep_gfp": ("GFP 标志与分配决策", [
        ("上下文语义", [("GFP_KERNEL", "可睡眠 · 进程上下文默认", None, "kzalloc(size, GFP_KERNEL);"),
                        ("GFP_ATOMIC", "中断/自旋锁内 · 不睡眠"), ("GFP_NOWAIT", "不睡眠 · 不触发回收"),
                        ("GFP_HIGHUSER", "用户态可移动页")]),
        ("分配器选择", [("kmalloc", "物理连续 · 小对象", "include/linux/slab.h"),
                        ("vmalloc", "虚拟连续 · 大块"), ("kmem_cache", "同构对象 · 精确控制"),
                        ("alloc_pages", "底层页分配")]),
        ("失败处理", [("NULL 检查", "必须判断 · 优雅降级", None, "if (!ptr) return -ENOMEM;"),
                      ("重试策略", "降级请求 · 缩小规模")]),
        ("内存压力", [("PF_MEMALLOC", "回收路径豁免"), ("__GFP_NOFAIL", "内核慎用 · 易死锁")]),
        ("源码入口", [("include/linux/gfp_types.h", "标志定义", "include/linux/gfp_types.h"),
                      ("mm/slab_common.c", "kmalloc 实现", "mm/slab_common.c")]),
    ]),
    "fdeep_barrier": ("内存屏障与内存序", [
        ("为什么需要", [("编译器重排", "优化改变访存顺序", None, "asm volatile(\"\" ::: \"memory\");"),
                        ("CPU 重排", "乱序执行 · 弱内存序"), ("可见性", "多核缓存一致性问题")]),
        ("屏障家族", [("barrier()", "编译器屏障 · CPU 照常", "include/linux/compiler.h"),
                      ("smp_mb/rmb/wmb", "全屏障/读屏障/写屏障"),
                      ("smp_acquire/release", "半屏障 · 锁原语 · 更高效")]),
        ("内核实例", [("自旋锁实现", "xchg + 屏障组合", "kernel/locking/spinlock.c"),
                      ("RCU 读侧", "编译屏障即可 · 为什么")]),
        ("实践纪律", [("优先用锁", "别手写屏障 · 除非你懂"), ("READ_ONCE 场景", "单次读 · 免屏障")]),
        ("源码入口", [("include/asm-generic/barrier.h", "屏障定义", "include/asm-generic/barrier.h"),
                      ("Documentation/memory-barriers.txt", "权威文档", "Documentation/memory-barriers.txt")]),
    ]),
    "fdeep_link": ("链接脚本与内核布局", [
        ("vmlinux.lds.S", [("布局总纲", "段顺序 · 对齐 · 符号", "arch/x86/kernel/vmlinux.lds.S"),
                           ("段与权限", "text 只读可执行 · rodata 只读 · data/bss 读写")]),
        ("__init 段", [("启动后回收", "init 代码/数据释放", None, "__init int foo_init(void)"),
                       ("__read_mostly", "热读数据 · 防伪共享")]),
        ("符号与定位", [("System.map", "地址↔符号 · Oops 解码", "scripts/mksysmap"),
                        ("kallsyms", "运行时符号表"), ("模块重定位", ".ko 加载时地址修正")]),
        ("观测工具", [("readelf/objdump", "段与符号分析", None, "readelf -S vmlinux"),
                      ("/proc/kallsyms", "运行时符号查询")]),
        ("源码入口", [("arch/x86/kernel/vmlinux.lds.S", "x86 链接脚本", "arch/x86/kernel/vmlinux.lds.S"),
                      ("scripts/mksysmap", "System.map 生成", "scripts/mksysmap")]),
    ]),
}
# 精选讲解与资源：key -> {"text": 讲解, "links": [(标题, URL), ...]}
NOTES = {
    "n_container_of": {"text": "container_of 是内核第一宏：给定结构体成员的指针，反推出宿主结构体的指针，实现\"嵌入容器\"设计。原理 = (char*)ptr - offsetof(type, member)，配合 __same_type 做编译期类型检查。理解它 = 理解内核所有链表/树/哈希的实现基础。",
        "links": [("源码实现(带行号)", "https://codebrowser.dev/linux/linux/include/linux/container_of.h.html#19"), ("LWN 内核数据结构系列(经典)", "https://lwn.net/Articles/337089/")]},
    "n_list": {"text": "list_head 是双向循环链表：空表自指、永不为 NULL，插入删除 O(1)。关键是\"嵌入\"——对象内嵌 struct list_head 成员，用 container_of 反推宿主。遍历用 list_for_each_entry，遍历中删除必须用 *_safe 版本。",
        "links": [("LWN: 内核链表入门(经典)", "https://lwn.net/Articles/337089/"), ("list.h 全部 API", "https://elixir.bootlin.com/linux/latest/source/include/linux/list.h")]},
    "n_kfifo": {"text": "kfifo 是无锁环形缓冲：只支持单生产者单消费者，读写各维护一个 index 即可免锁。内核用它做串口缓冲等场景。超过一对多就要加锁或换结构。",
        "links": [("kfifo.h 源码", "https://elixir.bootlin.com/linux/latest/source/include/linux/kfifo.h"), ("内核 API 文档", "https://docs.kernel.org/core-api/kernel-api.html")]},
    "n_rbtree": {"text": "红黑树保证插入/删除/查找 O(log n)，内核用于调度队列、VMA 区间、定时器等。学习重点是旋转与着色规则，建议对照 lib/rbtree.c 阅读（rb_insert_color/rb_erase）。",
        "links": [("内核 rbtree 文档", "https://docs.kernel.org/core-api/rbtree.html"), ("rbtree.c 实现", "https://elixir.bootlin.com/linux/latest/source/lib/rbtree.c")]},
    "n_xarray": {"text": "xarray 是稀疏数组/基数树的现代实现，取代了 radix tree。页缓存、文件偏移索引都在用它。API 以 xa_ 开头（xa_store/xa_load），支持 RCU 无锁读。",
        "links": [("LWN: xarray 简史", "https://lwn.net/Articles/745958/"), ("xarray 官方文档", "https://docs.kernel.org/core-api/xarray.html")]},
    "n_rhashtable": {"text": "rhashtable 是内核哈希表标配：动态扩容、无锁读（RCU）、链式冲突。网络栈路由/连接表都在用。需要关心哈希函数选择（jhash）与扩容时的遍历语义。",
        "links": [("rhashtable.c 实现", "https://elixir.bootlin.com/linux/latest/source/lib/rhashtable.c"), ("内核 API 文档", "https://docs.kernel.org/core-api/kernel-api.html")]},
    "n_gfp": {"text": "GFP 标志决定分配行为：GFP_KERNEL 可睡眠（仅进程上下文），GFP_ATOMIC 原子上下文不睡眠，GFP_NOWAIT 不睡眠不回收。选错标志轻则警告、重则死锁；分配结果必须检查 NULL。",
        "links": [("内存分配指南(官方)", "https://docs.kernel.org/core-api/memory-allocation.html"), ("gfp_types.h 标志定义", "https://elixir.bootlin.com/linux/latest/source/include/linux/gfp_types.h")]},
    "n_kref": {"text": "kref 是引用计数生命周期管理：kref_get 增加引用，kref_put 减到 0 时调用 release 回调。铁律：谁拿引用谁负责归还，防止 use-after-free；引用计数与锁要配合设计。",
        "links": [("kref 官方文档", "https://docs.kernel.org/core-api/kref.html"), ("kref.h 源码", "https://elixir.bootlin.com/linux/latest/source/include/linux/kref.h")]},
    "n_printk": {"text": "printk 是内核日志输出：支持级别（KERN_ERR/INFO/DEBUG）与格式扩展（%pK 指针脱敏、%pa 物理地址、%pS 符号化）。生产环境用动态调试（dynamic_debug）控制开关，避免刷屏。",
        "links": [("printk 基础文档", "https://docs.kernel.org/core-api/printk-basics.html"), ("printk.c 实现", "https://elixir.bootlin.com/linux/latest/source/kernel/printk/printk.c")]},
    "n_spinlock": {"text": "自旋锁忙等不睡眠，只适合极短临界区；持有期间禁止调用任何可能睡眠的函数（包括 kmalloc(GFP_KERNEL)）。中断上下文要用 spin_lock_irqsave/spin_unlock_irqrestore 保存中断状态。",
        "links": [("自旋锁官方文档", "https://docs.kernel.org/locking/spinlocks.html"), ("spinlock.h 源码", "https://elixir.bootlin.com/linux/latest/source/include/linux/spinlock.h")]},
    "n_mutex": {"text": "mutex 可睡眠，是进程上下文默认选择；自带优先级继承防止优先级反转。临界区长、可能睡眠（如等 IO）时用它。与自旋锁的选择：临界区多短、会不会睡眠。",
        "links": [("mutex 设计文档", "https://docs.kernel.org/locking/mutex-design.html"), ("mutex.c 实现", "https://elixir.bootlin.com/linux/latest/source/kernel/locking/mutex.c")]},
    "n_barrier": {"text": "内存屏障解决编译器/CPU 重排导致的可见性问题：barrier() 只挡编译器，smp_mb/rmb/wmb 挡 CPU。实践纪律：优先用锁和 READ_ONCE/WRITE_ONCE，别手写屏障——除非你真懂内存模型。",
        "links": [("内存屏障权威文档", "https://docs.kernel.org/memory-barriers.html"), ("barrier.h 源码", "https://elixir.bootlin.com/linux/latest/source/include/linux/barrier.h")]},
    "n_rcu": {"text": "RCU：读侧无锁、无原子操作、无屏障；写侧\"复制-修改-替换\"，等待宽限期（所有旧读者退出）后才释放旧数据。读多写少的场景（路由表、dcache）首选。经典文章：Paul McKenney 的 What is RCU。",
        "links": [("What is RCU(官方)", "https://docs.kernel.org/RCU/whatisRCU.html"), ("LWN: RCU 是什么(必读)", "https://lwn.net/Articles/262464/")]},
    "n_eevdf": {"text": "EEVDF（6.6+）取代 CFS：按虚拟运行时间在红黑树中取最左节点调度，引入延迟界限（lag）保证公平与低延迟。核心函数 pick_eevdf 在 kernel/sched/fair.c。面试高频：CFS 与 EEVDF 的区别。",
        "links": [("EEVDF 官方文档", "https://docs.kernel.org/scheduler/sched-eevdf.html"), ("Linux Magazine: A Fair Slice", "https://www.linux-magazine.com/Online/Features/EEVDF#1")]},
    "n_warn": {"text": "WARN_ON 打印警告+调用栈但不停止系统，用于\"理论上不该发生\"的路径；BUG_ON 直接崩溃（内核认为继续运行更危险）。用 WARN_ON_ONCE 防刷屏。写代码时用 WARN 不用 BUG。",
        "links": [("bug.h 源码", "https://elixir.bootlin.com/linux/latest/source/include/asm-generic/bug.h"), ("内核编码风格", "https://docs.kernel.org/process/coding-style.html")]},
    "n_lockdep": {"text": "lockdep 在运行时验证锁序：检测死锁、重复加锁、锁与上下文不匹配（如原子上下文睡眠）。开发期务必开启，能提前暴露 90% 的锁问题。看到 lockdep 报错先别慌，读调用链。",
        "links": [("lockdep 设计文档", "https://docs.kernel.org/locking/lockdep-design.html"), ("lockdep.c 源码", "https://elixir.bootlin.com/linux/latest/source/kernel/locking/lockdep.c")]},
    "n_kasan": {"text": "KASAN 编译期插桩，检测越界访问与 use-after-free，开发期利器（生产期开销大）。开启 CONFIG_KASAN 重新编译内核即可，崩在 KASAN 报告处就是 bug 现场。",
        "links": [("KASAN 官方文档", "https://docs.kernel.org/dev-tools/kasan.html"), ("调试工具总览", "https://docs.kernel.org/dev-tools/index.html")]},
    "n_module": {"text": "模块 = 可动态加载的内核代码：module_init/module_exit 注册入口，obj-m += 编译，insmod 装载、dmesg 验证、rmmod 卸载。LDD3（Linux Device Drivers 第三版）网上免费，是驱动入门圣经。",
        "links": [("内核模块构建文档", "https://docs.kernel.org/kbuild/modules.html"), ("LDD3 免费全书", "https://lwn.net/Kernel/LDD3/")]},
    "n_vfs": {"text": "VFS 是文件系统抽象层：super_block（文件系统实例）、inode（元数据）、dentry（路径组件）、file（打开实例）。所有文件系统实现这四件套的接口回调，路径解析是热点路径（dcache 缓存）。",
        "links": [("VFS 官方文档", "https://docs.kernel.org/filesystems/vfs.html"), ("namei.c 路径解析", "https://elixir.bootlin.com/linux/latest/source/fs/namei.c")]},
    "n_io_uring": {"text": "io_uring 用共享环形队列提交/收割 IO：SQE 提交请求、CQE 收割完成，免系统调用+免锁，吞吐碾压 epoll。现代存储/网络高性能应用标配。面试高频：io_uring vs epoll。",
        "links": [("io_uring 官方文档", "https://docs.kernel.org/io_uring/"), ("io_uring.c 源码", "https://elixir.bootlin.com/linux/latest/source/io_uring/io_uring.c")]},
    "n_bpf": {"text": "eBPF 允许在内核安全执行用户程序：观测（trace）、网络（XDP/TC）、安全（LSM）。BTF 提供类型信息。bpftrace 是上手最快的入口（一行脚本看系统调用）。内核开发者必学技能。",
        "links": [("BPF 官方文档", "https://docs.kernel.org/bpf/"), ("BCC/bpftrace 项目", "https://github.com/iovisor/bpftrace")]},
    "n_buddy": {"text": "伙伴系统按 2^order 页块管理物理内存：分配大块分裂、释放小块合并。per-CPU 页池加速热路径，迁移类型分组 + compaction 缓解碎片。理解它 = 理解物理内存管理的地基。",
        "links": [("内存管理文档", "https://docs.kernel.org/mm/index.html"), ("page_alloc.c 实现", "https://elixir.bootlin.com/linux/latest/source/mm/page_alloc.c")]},
    "n_bili": {"title": "🎬 视频资源", "text": "中文视频资源（B站）：① 3小时精通Linux内核（官方授权）② Linux内核实战教程 ③ 内核与内存调优保姆级教程。适合通勤/碎片时间建立整体感，系统性学习仍以文档+源码为主。",
        "links": [("3小时精通Linux内核", "https://www.bilibili.com/video/BV1e3cwz2Ewg/"), ("Linux内核实战教程", "https://www.bilibili.com/video/BV1U1UfBiEPr/"), ("内核与内存调优", "https://www.bilibili.com/video/BV1Xbp2zbEWE/")]},
}

# 代码补充库：f"{图key}|{条目名}" -> 真实内核代码片段（自动注入缺代码的条目）
CODE_EXTRA = {
    "gcc_ext|noreturn/format/unused": "__attribute__((noreturn)) void panic_fn(void);",
    "gcc_ext|offsetof": "off = offsetof(struct foo, bar);",
    "gcc_ext|__builtin_constant_p": "if (__builtin_constant_p(x)) fast_path(x);",
    "gcc_ext|__builtin_return_address": "caller = __builtin_return_address(0);",
    "gcc_ext|约束符": 'asm("add %1, %0" : "+r"(a) : "r"(b));',
    "gcc_ext|实例": 'asm volatile("mfence");',
    "gcc_ext|变长宏 ##__VA_ARGS__": '#define pr(fmt, ...) printk(fmt, ##__VA_ARGS__)',
    "macro|__VA_ARGS__": '#define pr(fmt, ...) printk(fmt, ##__VA_ARGS__)',
    "macro|BUILD_BUG_ON_ZERO": "BUILD_BUG_ON_ZERO(sizeof(int) != 4)",
    "macro|round_up/DIV_ROUND_UP": "sz = round_up(size, PAGE_SIZE);",
    "macro|IS_ALIGNED": "if (IS_ALIGNED(addr, 8)) ...",
    "macro|括号包裹": "#define MAX(a,b) ((a) > (b) ? (a) : (b))",
    "macro|宏 vs 函数": "static inline int add(int a, int b) { return a + b; }",
    "macro|include 守卫": "#ifndef _LINUX_FOO_H  /* ... */  #endif",
    "c_deep|整数提升": "char c = 200; int i = c; /* 先提升再运算 */",
    "c_deep|char 符号性": "signed char s; u8 u; /* 显式声明避免歧义 */",
    "c_deep|union 类型双关": "union { u32 v; u8 b[4]; } x = { .v = 0x12345678 };",
    "c_deep|位域": "struct { unsigned a : 3; unsigned b : 5; };",
    "c_deep|数组退化": "void f(int arr[]) { sizeof(arr); /* 是指针 8! */ }",
    "c_deep|指针算术越界": "if (p + n > end) return -EINVAL; /* 越界是 UB */",
    "c_deep|空指针解引用": "if (!ptr) return -EINVAL;",
    "c_deep|资源配对": "mutex_lock(&m); ... mutex_unlock(&m);",
    "c_deep|提前返回纪律": "if (ret) return ret; /* 扁平化错误路径 */",
    "c_deep|有符号溢出": "if (a > INT_MAX - b) return -EOVERFLOW;",
    "c_deep|除零/越界/未初始化": "if (b == 0) return -EINVAL;",
    "c_deep|严格别名违反": "memcpy(&u, &f, sizeof(u)); /* 避免别名 UB */",
    "memory_c|多级指针": "int **pp = &p; /* 链表删除场景 */",
    "memory_c|const 限定": "const char *p; char *const p; /* 两种语义 */",
    "memory_c|restrict": "void f(int *restrict a, int *restrict b);",
    "memory_c|段布局": "static int __read_mostly counter;",
    "memory_c|内核栈": "char buf[512]; /* 8KB 栈 · 禁止大数组 */",
    "memory_c|对齐与填充": "struct s __attribute__((aligned(64))) x;",
    "memory_c|位图 API": "DECLARE_BITMAP(bits, 64);",
    "link_mem|编译单元": "gcc -c foo.c -o foo.o",
    "link_mem|内部/外部链接": "static int local; /* 仅本编译单元可见 */",
    "link_mem|符号解析": "nm foo.o | grep ' U '",
    "link_mem|重定位": "objdump -r foo.o",
    "link_mem|stack protector": "CONFIG_STACKPROTECTOR_STRONG=y",
    "link_mem|栈帧布局": "gcc -fno-omit-frame-pointer -c foo.c",
    "link_mem|栈溢出原理": "char buf[4]; strcpy(buf, s); /* 覆写返回地址 */",
    "link_mem|局部性": "/* 相邻字段一起访问 · 缓存友好 */",
    "ds|hlist": "struct hlist_node { struct hlist_node *next, **pprev; };",
    "ds|增删改查": "list_add_tail(&n->list, &head);",
    "ds|maple tree": "mt_init(&mt); mas_store(&mas, idx, ptr);",
    "ds|嵌入 vs 指针": "struct obj { struct list_head node; };",
    "ds|生命周期": "kzalloc → INIT_LIST_HEAD → 使用 → list_del → kfree",
    "ds|RCU 变体": "hlist_nulls_add_head_rcu(&n->nulls, &h);",
    "ds_adv|哈希函数选择": "h = jhash2(words, nwords, seed);",
    "ds_adv|per-cpu 思想": "DEFINE_PER_CPU(int, counter); this_cpu_inc(counter);",
    "ds_adv|kmem_cache": "cache = kmem_cache_create(\"foo\", size, 0, 0, NULL);",
    "ds_adv|rbtree 场景": "rb_insert_color(&node->rb, &root);",
    "ds_adv|选择决策": "/* 读多写少→RCU · 区间查找→rbtree */",
    "api|负数错误码": "return -ENOMEM; /* 0 成功 负错误 */",
    "api|PTR_ERR/ERR_CAST": "ret = PTR_ERR(ptr);",
    "api|vmalloc": "ptr = vmalloc(size); vfree(ptr);",
    "api|kmem_cache": "obj = kmem_cache_alloc(cache, GFP_KERNEL);",
    "api|对称性": "/* get/put 成对 · 谁拿谁还 */",
    "api|格式扩展": 'pr_info("%pK %pa\\n", ptr, &phys);',
    "api|动态调试": "echo 'file drv.c +p' > /sys/kernel/debug/dynamic_debug/control",
    "api|strnlen/strscpy_pad": "n = strnlen(s, MAX_LEN);",
    "api|memcpy vs memmove": "memmove(dst, src, n); /* 允许重叠 */",
    "api|FORTIFY 联动": "CONFIG_FORTIFY_SOURCE=y  /* 字符串越界检测 */",
    "sync_base|原子位": "set_bit(3, &flags);",
    "sync_base|atomic_long_t": "atomic_long_inc(&counter);",
    "sync_base|数据竞争": "/* CONFIG_KCSAN=y 运行时检测 */",
    "sync_base|选择依据": "/* 短且不睡眠→自旋 · 长或睡眠→互斥 */",
    "sync_base|kfifo 单生产者": "kfifo_in(&fifo, buf, n);",
    "sync_base|seqlock": "write_seqlock(&seql); ... write_sequnlock(&seql);",
    "debug|dump_stack": "dump_stack(); /* 打印调用栈取证 */",
    "debug|panic": 'panic("fatal: %s\\n", msg);',
    "debug|调试点选择": 'pr_debug("enter %s\\n", __func__);',
    "debug|KCSAN": "CONFIG_KCSAN=y /* 数据竞争检测 */",
    "debug|UBSAN": "CONFIG_UBSAN=y /* 未定义行为检测 */",
    "debug|崩溃输出": "/* RIP: 0010:func+0x12/0x30 */",
    "debug|解码": "./scripts/decode_stacktrace.sh < oops.txt",
    "debug|常规手段": "addr2line -e vmlinux 0xffffffff81001234",
    "module_dev|元信息": 'MODULE_AUTHOR("Me"); MODULE_DESCRIPTION("Demo");',
    "module_dev|Kconfig": 'config FOO\n\ttristate "Foo support"',
    "module_dev|insmod/rmmod/lsmod": "insmod hello.ko && dmesg | tail",
    "module_dev|modprobe": "modprobe hello debug=1",
    "module_dev|proc/sysfs": 'kobject_create_and_add("foo", kernel_kobj);',
}

def materialize(key, cols, found):
    """把 CODE_EXTRA 中缺失的代码片段注入条目（found: 第4位；normal: 第6位）"""
    new_cols = []
    for (cname, items) in cols:
        new_items = []
        for it in items:
            lst = list(it)
            code_idx = 3 if found else 5
            have = len(lst) > code_idx and lst[code_idx]
            if not have:
                extra = CODE_EXTRA.get(f"{key}|{lst[0]}")
                if extra:
                    while len(lst) <= code_idx:
                        lst.append(None)
                    lst[code_idx] = extra
            new_items.append(tuple(lst))
        new_cols.append((cname, new_items))
    return new_cols

ROADMAP = [
    ("阶段 0 · 地基", "内核 C 语言地基：语法之外的一切重难点", ["GNU C 扩展（attribute/container_of/内联汇编）", "指针与内存模型（段/对齐/大小端/位操作）", "内核数据结构（链表/红黑树/xarray/kfifo）", "API 惯例（错误码/GFP/kref/printk）", "并发地基（原子/屏障/锁）", "调试断言（WARN_ON/lockdep/KASAN）", "写出第一个内核模块"], "能独立写出并装载一个 hello 模块", "foundation"),
    ("阶段 1 · 内核入门", "自己编译并运行一个内核", ["搭建编译环境 · 配置 Kconfig", "写一个 hello 字符驱动", "proc/sysfs 读写", "学会 dmesg/模块管理"], "insmod 自己的驱动并 dmesg 可见", "xcut"),
    ("阶段 2 · 核心子系统", "打通 进程→内存→同步 主线", ["进程与调度（EEVDF）", "内存管理（页表/伙伴/slab）", "同步机制（锁/RCU）"], "能讲清 fork 的写时复制", "proc"),
    ("阶段 3 · 文件与网络", "理解数据如何落盘与传输", ["VFS → ext4 → 回写", "socket → TCP/IP → NAPI"], "能画出 read() 的完整调用链", "fst"),
    ("阶段 4 · 深水区", "并发极致与内核编程", ["RCU 原理", "eBPF 观测编程", "io_uring 异步", "KVM 虚拟化"], "用 bpftrace 追踪系统调用延迟", "ipc_sync"),
    ("阶段 5 · 实战输出", "性能调优与源码贡献", ["perf 火焰图定位瓶颈", "ftrace/kprobe 动态追踪", "阅读主线源码 + 提交补丁"], "给开源项目提一个补丁", "resources"),
]

# 资源与误区
RES = {
    "官方与社区": [("docs.kernel.org", "内核官方文档 · 权威第一手"), ("kernelnewbies.org", "内核新手社区 · 周报"), ("LWN.net", "深度内核文章 · 必订"), ("LKML", "内核邮件列表 · 补丁主战场")],
    "书籍推荐(按序)": [("《Linux内核设计与实现》LKD", "最薄最顺 · 先读这本"), ("《深入理解Linux内核》ULK", "大部头 · 原理细节词典"), ("《奔跑吧Linux内核》", "动手实验型 · 配 qemu"), ("《Linux设备驱动》LDD3", "驱动入门经典")],
    "常见误区纠正": [("误区: 从头读源码", "正确: 按子系统+调用链读，先跑起来"), ("误区: 看视频就会了", "正确: 必须自己编译/改/验证"), ("误区: 内核就是老古董", "正确: io_uring/eBPF 每年都在革新"), ("误区: 内存=malloc", "正确: 页表/伙伴/slab/回收是一整套"), ("误区: 驱动=写外设", "正确: 驱动模型/总线/PM 是主体")],
    "面试高频(内核向)": [("fork 之后发生了什么(写时复制)"), ("自旋锁 vs 互斥锁怎么选"), ("中断上下半部为什么拆分"), ("RCU 为什么读侧快"), ("缺页异常处理流程"), ("OOM killer 如何选进程"), ("为什么需要系统调用这层"), ("io_uring 比 epoll 强在哪"), ("eBPF 的工作原理"), ("进程和线程的内核差异")],
}

SUB_NAMES = {k: v[0] for k, v in {**SUBS, **DEEP, **FOUND, **FDEEP}.items()}
SUB_NAMES.update({"roadmap": "学习路线图", "resources": "资源与误区", "foundation": "内核 C 地基"})
GROUP_COLORS = {
    "usr": ("#2471a3", "#7fb3e8"), "sci": ("#27ae60", "#8ce0ac"),
    "proc": ("#d35400", "#ffa64d"), "mem": ("#b7950b", "#f1c40f"),
    "fst": ("#1e8449", "#58d68d"), "net": ("#1a5276", "#5dade2"),
    "ipc_sync": ("#6c3483", "#af7ac5"), "irq_t": ("#c0392b", "#f1948a"),
    "drv_v": ("#0e6655", "#48c9b0"), "sec_p": ("#515a5a", "#aeb6bf"),
    "xcut": ("#148f77", "#76d7c4"), "arch": ("#7d6608", "#f4d03f"),
    "hw": ("#4d5656", "#99a3a4"), "deep": ("#1f3a5f", "#5b8dd9"),
}
LEGEND = [("usr", "用户空间"), ("sci", "系统调用"), ("proc", "进程与调度"), ("mem", "内存管理"),
          ("fst", "文件系统"), ("net", "网络"), ("ipc_sync", "并发与通信"), ("irq_t", "中断与时间"),
          ("drv_v", "驱动与虚拟化"), ("sec_p", "安全与电源"), ("xcut", "横切"), ("arch", "体系结构"), ("hw", "硬件")]

C = dict(bg="#15181c", card="#1f2429", card_border="#2c333d", line="#3a4350",
         text="#e8edf3", dim="#93a0b0", accent="#4f8cff", accent_t="#9fc3ff",
         layer="#20252b", layer_border="#39424e", item="#262c33", item_border="#3a4350",
         title_bar="#1c2e4a", title_border="#3b5b8a", col_head="#2a323d", col_border="#41506a",
         warn_fill="#3d3413", warn_border="#8a6d1f", warn_text="#f0d98c")

def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def build_svg(W, H_, parts):
    return ("\n".join([f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H_}" width="{W}" height="{H_}">',
                       f'<rect width="{W}" height="{H_}" fill="{C["bg"]}"/>'] + parts + ["</svg>"]))

def srect(x, y, w, h, fill, stroke, sw=2, rx=12, extra=""):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{extra}/>'

def stext(cx, cy, s, fs, fill=C["text"], anchor="middle", bold=False):
    w = ' font-weight="700"' if bold else ""
    return f'<text x="{cx}" y="{round(cy + fs * 0.35, 1)}" font-family="{FAM}" font-size="{fs}" fill="{fill}" text-anchor="{anchor}"{w}>{esc(s)}</text>'

def sltext(x, y, s, fs, fill=C["text"], bold=False):
    return stext(x, y, s, fs, fill, anchor="start", bold=bold)

def title_bar(W, title, tag=""):
    out = [srect(30, 26, W - 60, 64, C["title_bar"], C["title_border"], rx=12),
           stext(W / 2, 58, title + (("  ·  " + tag) if tag else ""), 25, "#FFFFFF", bold=True)]
    return out

def esc_attr(s):
    return esc(s).replace('"', "&quot;")

def item_box(x, y, w, name, detail, target=None, src=None, code=None, h=44, note=None):
    extra = ' class="cbox"'
    if target:
        extra += f' data-target="{target}"'
    if src:
        extra += f' data-src="{src}"'
    if note:
        extra += f' data-note="{note}"'
    extra += f' data-name="{esc_attr(name)}" data-detail="{esc_attr(detail)}"'
    if code:
        extra += f' data-code="{esc_attr(code)}"'
        import re as _re
        m = _re.search(r"([A-Za-z_][A-Za-z0-9_]*)", code)
        if m:
            extra += f' data-sym="{m.group(1)}"'
    out = [srect(x, y, w, h, C["item"], "#3d4754" if (target or src) else C["item_border"], 1.5, 8, extra=extra)]
    if target:
        out.append(f"<title>点击进入专题：{detail}</title>")
    elif src:
        out.append(f"<title>点击打开源码：https://elixir.bootlin.com/linux/latest/source/{src}</title>")
    color = "#7ab8ff" if target else ("#8ce0ac" if src else C["text"])
    marker = ("🔗 " if target else "") + ("📄 " if src else "")
    if code:
        out.append(sltext(x + 14, y + 15, marker + name, 16, color, bold=True))
        out.append(f'<text x="{x + 14}" y="{round(y + 34, 1)}" font-family="Consolas, monospace" font-size="13" fill="#f0d98c" text-anchor="start">{esc(code)}</text>')
        out.append(sltext(x + 14, y + 52, detail, 12.5, C["dim"]))
    else:
        out.append(sltext(x + 14, y + 18, marker + name, 16.5, color, bold=True))
        out.append(sltext(x + 14, y + 36, detail, 13, C["dim"]))
    if note and ":" not in note:
        out.append(f'<g class="note-btn" data-note="{note}"><text x="{x + w - 16}" y="{y + h / 2 + 4}" text-anchor="middle" font-family="{FAM}" font-size="14" fill="#7ab8ff">ⓘ</text></g>')
    return out

def autolink(D):
    """把标题为“源码入口”的列自动转为可点击源码链接（4 元组: name, detail, None, src）"""
    for key, (title, cols) in D.items():
        for ci, (cname, items) in enumerate(cols):
            if cname == "源码入口":
                cols[ci] = (cname, [it if len(it) > 2 else (it[0], it[1], None, it[0]) for it in items])

autolink(SUBS)
autolink(DEEP)

def sub_svg(key, title, cols, found=False):
    cols = materialize(key, cols, found)
    CW, GAP = 470, 24
    per_row = 3
    has_code = any(len(it) > (3 if found else 5) and it[3 if found else 5] for c in cols for it in c[1])
    step = 64 if has_code else 56
    col_h = lambda items: 52 + 8 + len(items) * step + 16
    rows = [cols[i:i + per_row] for i in range(0, len(cols), per_row)]
    row_hs = [max(col_h(c[1]) for c in r) for r in rows]
    W = 70 + per_row * (CW + GAP) + 10
    H_ = 116 + sum(row_hs) + 20 * len(rows) + 24
    fill, border = GROUP_COLORS.get(key, GROUP_COLORS["deep"])
    P = title_bar(W, title)
    P.append(stext(72, 58, "●", 18, border))
    y = 126
    for r in rows:
        x = 50
        for ci, (cname, items) in enumerate(r):
            h = row_hs[0]
            P.append(srect(x, y, CW, h, C["card"], C["card_border"], 1.5))
            P.append(srect(x + 10, y + 12, CW - 20, 34, C["col_head"], C["col_border"], 1.5, 8))
            P.append(sltext(x + 24, y + 30, cname, 19, C["accent_t"], bold=True))
            iy = y + 60
            for ii, it in enumerate(items):
                if found:
                    name, detail = it[0], it[1]
                    src = it[2] if len(it) > 2 else None
                    code = it[3] if len(it) > 3 else None
                    target = it[4] if len(it) > 4 else None
                    note = it[5] if len(it) > 5 else None
                else:
                    name, detail = it[0], it[1]
                    target = it[2] if len(it) > 2 else None
                    src = it[3] if len(it) > 3 else None
                    note = it[4] if len(it) > 4 else None
                    code = it[5] if len(it) > 5 else None
                ih = 58 if code else 44
                if not note and not target and not src:
                    note = f"{key}:{ci}:{ii}"  # 普通条目：自动打开讲解抽屉
                P.extend(item_box(x + 10, iy, CW - 20, name, detail, target, src, code, ih, note))
                iy += (ih + 6)
            x += CW + GAP
        y += row_hs[0] + 20
    return build_svg(W, H_, P)

def roadmap_svg():
    W, H_ = 1500, 130 + len(ROADMAP) * 190 + 40
    P = title_bar(W, "Linux 内核学习路线图", "")
    y = 120
    for i, (stage, goal, items, verify, target) in enumerate(ROADMAP):
        extra = f' class="cbox" data-target="{target}"' if target else ""
        P.append(srect(60, y, W - 120, 170, C["card"], "#3d4754" if target else C["card_border"], 1.5, 12, extra=extra))
        if target:
            P.append(f"<title>点击进入：{SUB_NAMES[target]} 子知识结构图</title>")
        P.append(sltext(90, y + 32, stage, 22, "#7ab8ff", bold=True))
        P.append(sltext(90, y + 60, "目标：" + goal, 16, C["text"]))
        P.append(sltext(90, y + 88, "内容：" + " ｜ ".join(items), 14, C["dim"]))
        P.append(sltext(90, y + 116, "✅ 验证：" + verify, 14, "#8ce0ac"))
        if target:
            P.append(sltext(W - 110, y + 150, "进入 →", 14, "#7ab8ff"))
        if i < len(ROADMAP) - 1:
            P.append(f'<line x1="{W/2}" y1="{y+172}" x2="{W/2}" y2="{y+188}" stroke="#4a5564" stroke-width="3"/>')
        y += 190
    return build_svg(W, H_, P)

def foundation_svg():
    W, H_ = 1560, 1180
    P = title_bar(W, "🧱 内核 C 语言地基（阶段 0）", "")
    P.append(stext(W / 2, 120, "点击任意板块进入完整子图 · 每个知识点配有内核代码实例与源码链接", 17, C["dim"]))
    items = [
        ("gcc_ext", "GNU C 扩展", "attribute · typeof · container_of · 语句表达式 · 内建函数 · 内联汇编", "#7d6608", "#f4d03f"),
        ("macro", "预处理器与宏工程", "#/## 运算符 · BUILD_BUG_ON · min_t · FIELD_GET · 宏陷阱 · 头文件纪律", "#7d6608", "#f4d03f"),
        ("c_deep", "C 语言深水区", "整数提升 · 符号陷阱 · 柔性数组 · union 双关 · goto err · UB 清单", "#a04000", "#f0b27a"),
        ("memory_c", "指针与内存模型", "多级指针 · 段布局 · 对齐填充 · 大小端 · 位操作", "#1a5276", "#5dade2"),
        ("link_mem", "编译链接与内存纵深", "四阶段 · 链接脚本 · LTO · FORTIFY · 栈帧 ABI · 缓存行 · 内存序", "#1a5276", "#5dade2"),
        ("ds", "内核数据结构", "list_head · hlist · rbtree · xarray · maple tree · kfifo · idr", "#6c3483", "#af7ac5"),
        ("ds_adv", "数据结构进阶", "rhashtable · llist · waitqueue · completion · mempool · 演进脉络", "#6c3483", "#af7ac5"),
        ("api", "API 惯例与内存", "错误码 · GFP · kmalloc/vmalloc · kmem_cache · kref · 字符串安全 · printk", "#1e8449", "#58d68d"),
        ("sync_base", "并发地基", "atomic · READ_ONCE · 屏障 · 自旋锁 · 互斥锁 · 无锁模式", "#c0392b", "#f1948a"),
        ("debug", "调试与断言", "WARN_ON · pr_* · lockdep · KASAN · KCSAN · Oops 解读", "#148f77", "#76d7c4"),
        ("module_dev", "模块开发", "module_init · Kbuild · insmod · file_operations · proc/sysfs", "#0e6655", "#48c9b0"),
    ]
    BW, BH, GX, GY = 700, 190, 60, 160
    for i, (key, t, desc, fill, border) in enumerate(items):
        col, row = i % 2, i // 2
        x = GX + col * (BW + 40)
        y = GY + row * (BH + 26)
        P.append(srect(x, y, BW, BH, fill, border, 2.5, extra=f' class="cbox" data-target="{key}"'))
        P.append(f"<title>点击进入：{t} 完整子图</title>")
        P.append(sltext(x + 24, y + 40, t + "  ▶", 23, "#FFFFFF", bold=True))
        P.append(sltext(x + 24, y + 82, desc, 14.5, "#EAF2FC"))
        P.append(sltext(x + BW - 90, y + BH - 26, "进入 →", 14, "#FFFFFF"))
    y = GY + 6 * (BH + 26) - 6
    P.append(srect(60, y, W - 120, 46, C["card"], C["card_border"], 1.5, 10))
    P.append(stext(W / 2, y + 28, "推荐学习顺序：宏工程 → GNU C 扩展 → C 深水区 → 指针与内存 → 编译链接 → 数据结构 → 进阶 → API → 并发 → 调试 → 写模块", 15, "#f0d98c"))
    return build_svg(W, H_, P)

def resources_svg():
    cols = list(RES.items())
    CW, GAP = 560, 30
    per_row = 2
    rows = [cols[i:i + per_row] for i in range(0, len(cols), per_row)]
    col_h = lambda items: 52 + len(items) * 54 + 20
    row_hs = [max(col_h(c[1]) for c in r) for r in rows]
    W = 70 + per_row * (CW + GAP) + 10
    H_ = 116 + sum(row_hs) + 20 * len(rows) + 24
    P = title_bar(W, "学习资源金矿 · 误区纠正 · 面试高频（打破信息差）", "")
    y = 126
    for r in rows:
        x = 50
        for (cname, items) in r:
            h = row_hs[0]
            P.append(srect(x, y, CW, h, C["card"], C["card_border"], 1.5))
            P.append(srect(x + 10, y + 12, CW - 20, 34, C["col_head"], C["col_border"], 1.5, 8))
            P.append(sltext(x + 24, y + 30, cname, 19, C["accent_t"], bold=True))
            iy = y + 60
            for it in items:
                if isinstance(it, tuple):
                    n, d = it
                    P.append(srect(x + 10, iy, CW - 20, 44, C["item"], C["item_border"], 1.5, 8))
                    P.append(sltext(x + 24, iy + 18, n, 16, C["text"], bold=True))
                    P.append(sltext(x + 24, iy + 36, d, 13, C["dim"]))
                else:
                    P.append(srect(x + 10, iy, CW - 20, 44, "#2a2438", "#6c3483", 1.5, 8))
                    P.append(sltext(x + 24, iy + 27, "★ " + it, 15.5, "#cf9ff2"))
                iy += 54
            x += CW + GAP
        y += row_hs[0] + 20
    return build_svg(W, H_, P)

# ================= 总览图（与 v2 相同，略改标题） =================
W, H_ = 2600, 2160
P = [stext(W / 2, 46, "Linux 内核知识结构图", 44, "#FFFFFF", bold=True),
     stext(W / 2, 92, "点击任意彩色框图进入子知识结构图 · 🔗 条目可进入专题深挖 · 滚轮缩放 · 拖拽平移 · Esc 返回", 19, C["dim"])]
LX, LW = 100, 2400
P.append(srect(LX, 126, LW, 180, C["layer"], C["layer_border"], 3, 16))
P.append(sltext(LX + 20, 138, "① 用户空间 User Space", 27, C["accent_t"], bold=True))
usr = [("应用程序 / 服务", "Apache · MySQL · GUI"), ("标准 C 库", "glibc / musl · 系统调用封装"),
       ("Shell 与系统工具", "bash · 常用命令 · 工具链"), ("运行时", "JVM / 语言运行时 / 容器编排")]
for i, (n, d) in enumerate(usr):
    x = LX + 40 + i * 590
    P.append(srect(x, 181, 560, 105, *GROUP_COLORS["usr"], extra=' class="cbox" data-target="usr"'))
    P.append("<title>点击进入：用户空间 子知识结构图</title>")
    P.append(stext(x + 280, 219, n, 25, "#FFFFFF", bold=True))
    P.append(stext(x + 280, 259, d, 17, "#EAF2FC"))
P.append(srect(950, 336, 700, 110, *GROUP_COLORS["sci"], extra=' class="cbox" data-target="sci"'))
P.append("<title>点击进入：系统调用 子知识结构图</title>")
P.append(stext(1300, 371, "② 系统调用接口 SCI + vDSO", 25, "#FFFFFF", bold=True))
P.append(stext(1300, 408, "open · read · write · fork · execve · mmap · ioctl · epoll · io_uring", 17, "#EAF6EF"))
P.append(stext(1300, 431, "vDSO 快速路径 · 软中断陷入 (syscall / int 0x80)", 17, "#EAF6EF"))
P.append(srect(LX, 476, LW, 1330, "#171b1f", C["layer_border"], 3, 16))
P.append(sltext(LX + 20, 488, "③ 内核核心 Kernel Core（内核态：Ring0 / EL1）", 27, C["accent_t"], bold=True))
groups = [
    ("proc", "进程与调度", [("进程管理", "task_struct · fork/exit · 线程 · namespace"), ("调度器", "EEVDF(CFS) · 实时/Deadline · 负载均衡 · PREEMPT_RT")]),
    ("mem", "内存管理", [("虚拟内存", "页表 · MMU · 缺页 · COW"), ("物理内存", "伙伴系统 · slab/slub · CMA · HugePage"), ("页缓存与回收", "page cache · swap/zram · KSM · OOM")]),
    ("fst", "文件系统与存储", [("VFS 抽象", "dentry · inode · file · super_block"), ("页缓存与块层", "address_space · 回写 · bio · io_uring"), ("具体文件系统", "ext4 · xfs · btrfs · proc/sysfs · tmpfs")]),
    ("net", "网络协议栈", [("网络分层", "socket · TCP/UDP · IP · 邻居子系统"), ("转发与过滤", "netfilter/iptables · TC · 路由"), ("高速数据面", "NAPI · XDP · eBPF · DPDK")]),
    ("ipc_sync", "并发与通信", [("进程间通信", "管道 · 信号 · SysV · futex · socket"), ("同步机制", "原子 · 自旋锁 · 互斥锁 · RCU · seqlock · 内存屏障")]),
    ("irq_t", "中断与时间", [("中断子系统", "上半部/下半部 · softirq · workqueue · threaded IRQ"), ("时间管理", "jiffies · hrtimer · clockevent · NO_HZ")]),
    ("drv_v", "驱动与虚拟化", [("设备驱动", "字符/块/网络 · 驱动模型 · 设备树/ACPI · DMA"), ("虚拟化与容器", "KVM · virtio · vfio · cgroup v2 · namespace")]),
    ("sec_p", "安全与电源", [("安全框架", "LSM · SELinux/AppArmor · lockdown · 密钥环"), ("电源管理", "cpuidle · cpufreq · runtime PM · suspend")]),
]
COL_X, COL_W, GAPX = 140, 1130, 60
ROW_Y, ROW_H = 566, 215
for gi, (key, gname, items) in enumerate(groups):
    col, row = gi % 2, gi // 2
    x = COL_X + col * (COL_W + GAPX)
    y = ROW_Y + row * (ROW_H + 20)
    fill, border = GROUP_COLORS[key]
    P.append(srect(x, y, COL_W, ROW_H, fill, border, 2.5, extra=f' class="cbox" data-target="{key}"'))
    P.append(f"<title>点击进入：{gname} 子知识结构图</title>")
    P.append(sltext(x + 16, y + 30, gname + "  ▶", 22, "#FFFFFF", bold=True))
    total = len(items) * 48 + (len(items) - 1) * 10
    sy = y + 44 + (208 - total) // 2
    for (n, d) in items:
        P.append(srect(x + 14, sy, COL_W - 28, 48, C["item"], "#3d4754", 1.5, 8))
        P.append(stext(x + 30, sy + 26, n, 20, C["text"], anchor="start", bold=True))
        P.append(stext(x + COL_W - 30, sy + 26, d, 16, C["dim"], anchor="end"))
        sy += 58
P.append(srect(140, 1626, 2320, 170, "#171b1f", C["layer_border"], 2, 12))
P.append(sltext(160, 1638, "横切关注点（贯穿所有子系统）", 23, C["accent_t"], bold=True))
xc = [("xcut", "内核初始化", "start_kernel → init 进程"), ("xcut", "调试与观测", "printk · ftrace · kprobe · perf · eBPF/BTF · kgdb · KUnit"), ("xcut", "构建与开发", "Kconfig · Kbuild · LLVM/Clang · 编码规范")]
for i, (key, n, d) in enumerate(xc):
    x = 160 + i * 780
    fill, border = GROUP_COLORS[key]
    P.append(srect(x, 1666, 740, 110, fill, border, 2, extra=f' class="cbox" data-target="{key}"'))
    P.append("<title>点击进入：横切关注点 子知识结构图</title>")
    P.append(stext(x + 370, 1711, n + "  ▶", 23, "#FFFFFF", bold=True))
    P.append(stext(x + 370, 1751, d, 16, "#E8F6F2"))
P.append(srect(LX, 1826, LW, 140, C["layer"], C["layer_border"], 3, 16))
P.append(sltext(LX + 20, 1838, "④ 体系结构层 arch/", 27, C["accent_t"], bold=True))
P.append(srect(850, 1866, 900, 80, *GROUP_COLORS["arch"], extra=' class="cbox" data-target="arch"'))
P.append("<title>点击进入：体系结构层 子知识结构图</title>")
P.append(stext(1300, 1896, "x86 / ARM / RISC-V  ▶", 23, "#FFFFFF", bold=True))
P.append(stext(1300, 1926, "启动汇编 · 页表/MMU · APIC/GIC · 原子与屏障实现", 16, "#FBF6E0"))
P.append(srect(LX, 1986, LW, 140, C["layer"], C["layer_border"], 3, 16))
P.append(sltext(LX + 20, 1998, "⑤ 硬件层 Hardware", 27, C["accent_t"], bold=True))
P.append(srect(850, 2026, 900, 80, *GROUP_COLORS["hw"], extra=' class="cbox" data-target="hw"'))
P.append("<title>点击进入：硬件层 子知识结构图</title>")
P.append(stext(1300, 2056, "CPU · 内存 · 磁盘 · 网卡 · 外设  ▶", 23, "#FFFFFF", bold=True))
P.append(stext(1300, 2086, "中断控制器 · DMA · 总线 · 时钟", 16, "#EDEFF2"))
OVERVIEW_SVG = build_svg(W, H_, P)

# ================= HTML =================
CSS = """
:root { --bg:#121418; --card:#1d2126; --border:#2a3038; --border2:#333b46; --text:#e8edf3; --dim:#93a0b0; --accent:#4f8cff; }
* { box-sizing: border-box; }
body { margin:0; font-family:"Microsoft YaHei","PingFang SC",sans-serif; color:var(--text);
  background:radial-gradient(1100px 500px at 50% -8%, #1c2740 0%, var(--bg) 55%); min-height:100vh; }
header { position:sticky; top:0; z-index:50; display:flex; align-items:center; gap:12px; padding:11px 26px;
  background:rgba(18,20,24,.85); backdrop-filter:blur(12px); border-bottom:1px solid var(--border); flex-wrap:wrap; }
header .logo { font-size:21px; font-weight:800; background:linear-gradient(90deg,#7ab8ff,#e8edf3);
  -webkit-background-clip:text; background-clip:text; color:transparent; white-space:nowrap; }
header .crumb { color:var(--dim); font-size:13px; }
header .crumb a { color:var(--accent); text-decoration:none; cursor:pointer; }
header .crumb b { color:var(--text); }
header .spacer { flex:1; }
.btn { border:1px solid var(--border2); background:#242a32; color:var(--text); border-radius:10px; padding:7px 15px;
  font-size:13px; cursor:pointer; transition:all .15s; white-space:nowrap; }
.btn:hover { background:#2d3540; border-color:var(--accent); color:#fff; }
.btn.primary { background:var(--accent); border-color:var(--accent); color:#fff; font-weight:700; }
.btn.primary:hover { filter:brightness(1.12); }
.btn.gold { border-color:#8a6d1f; color:#f0d98c; }
main { max-width:1720px; margin:0 auto; padding:20px 24px 40px; }
.hero { text-align:center; margin:6px 0 18px; }
.hero h1 { margin:0; font-size:33px; font-weight:800; letter-spacing:1px;
  background:linear-gradient(90deg,#7ab8ff 0%,#b8d8ff 45%,#e8edf3 100%); -webkit-background-clip:text; background-clip:text; color:transparent; }
.hero p { margin:8px 0 0; color:var(--dim); font-size:14px; }
.stats { display:flex; gap:10px; justify-content:center; margin:14px 0 4px; flex-wrap:wrap; }
.stat { background:var(--card); border:1px solid var(--border); border-radius:999px; padding:5px 16px; font-size:12.5px; color:var(--dim); }
.stat b { color:var(--accent); }
.legend { display:flex; gap:8px; flex-wrap:wrap; justify-content:center; margin:12px 0 16px; }
.chip { display:inline-flex; align-items:center; gap:7px; background:var(--card); border:1px solid var(--border);
  border-radius:999px; padding:5px 13px; font-size:12.5px; color:var(--dim); cursor:pointer; transition:all .15s; user-select:none; }
.chip:hover { border-color:var(--accent); color:var(--text); transform:translateY(-1px); }
.chip i { width:10px; height:10px; border-radius:3px; display:inline-block; }
.card { background:var(--card); border:1px solid var(--border); border-radius:14px; box-shadow:0 10px 30px rgba(0,0,0,.35); padding:16px; }
.stage { position:relative; overflow:hidden; border:1px solid var(--border); border-radius:10px; background:#15181c; cursor:grab; height:76vh; }
.stage.dragging { cursor:grabbing; }
.inner { transform-origin:0 0; }
.inner svg { display:block; max-width:none; }
.cbox { cursor:pointer; transition:filter .15s; }
.cbox:hover { filter:brightness(1.22) drop-shadow(0 0 10px rgba(120,180,255,.35)); }
.note-btn { cursor:pointer; }
.note-btn:hover text { fill:#ffffff; }
.drawer { position:fixed; top:0; right:0; width:460px; max-width:94vw; height:100vh; z-index:200;
  background:#171b20; border-left:1px solid #2c333d; box-shadow:-14px 0 44px rgba(0,0,0,.55);
  transform:translateX(105%); transition:transform .22s ease; display:flex; flex-direction:column; }
.drawer.open { transform:translateX(0); }
.drawer-head { display:flex; align-items:center; justify-content:space-between; padding:14px 18px;
  border-bottom:1px solid #2c333d; background:#1a1e24; }
.drawer-head b { color:#7ab8ff; font-size:16px; }
.drawer-head button { background:none; border:none; color:#93a0b0; font-size:18px; cursor:pointer; padding:4px 8px; }
.drawer-head button:hover { color:#fff; }
#d-body { padding:16px 18px; overflow-y:auto; flex:1; font-size:14px; line-height:1.75; }
.ndet { color:#93a0b0; margin-bottom:10px; }
.ncode { background:#121418; border:1px solid #2c333d; border-radius:8px; padding:10px 12px; color:#f0d98c;
  font-family:Consolas,monospace; font-size:12.5px; overflow-x:auto; white-space:pre-wrap; margin:8px 0; }
.ntext { color:#e8edf3; }
.nsec { color:#9fc3ff; font-weight:700; margin:14px 0 6px; font-size:13px; }
.nlink { display:block; color:#7ab8ff; text-decoration:none; padding:7px 12px; margin:5px 0;
  background:#242a32; border:1px solid #2c333d; border-radius:8px; font-size:13px; }
.nlink:hover { border-color:#4f8cff; background:#2a323d; }
footer { text-align:center; color:#5d6875; font-size:12px; margin:24px 0 8px; }
.subview, #view-overview { animation:fadein .18s ease; }
@keyframes fadein { from { opacity:0; transform:translateY(4px); } to { opacity:1; transform:none; } }
"""

JS = r"""
function attachZoom(stageId, svgW, svgH){
  var stage = document.getElementById(stageId);
  var inner = stage.querySelector('.inner');
  var scale = 1, tx = 0, ty = 0;
  function apply(){ inner.style.transform = 'translate(' + tx + 'px,' + ty + 'px) scale(' + scale + ')'; }
  function zoomAt(mx, my, f){
    var ns = Math.max(0.1, Math.min(8, scale * f));
    tx = mx - (mx - tx) * (ns / scale);
    ty = my - (my - ty) * (ns / scale);
    scale = ns; apply();
  }
  stage.zoomBy = function(f){ var r = stage.getBoundingClientRect(); zoomAt(r.width/2, r.height/2, f); };
  stage.setZoom = function(s){ var r = stage.getBoundingClientRect(); zoomAt(r.width/2, r.height/2, s/scale); };
  stage.fit = function(){
    var r = stage.getBoundingClientRect();
    if (r.width === 0 || r.height === 0) { stage._needFit = true; return; }
    stage._needFit = false;
    var s = Math.min(r.width / svgW, r.height / svgH, 1);
    scale = s; tx = (r.width - svgW * s) / 2; ty = (r.height - svgH * s) / 2; apply();
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
  stage.fit();
  return stage;
}
var SUB_NAMES = %SUBNAMES%;
var STACK = ['overview'];
function current(){ return STACK[STACK.length - 1]; }
function showView(key){
  if (key === current()) return;
  STACK.push(key); render();
}
function showSub(key){ showView(key); }
function back(){ if (STACK.length > 1) { STACK.pop(); render(); } }
function showOverview(){ STACK = ['overview']; render(); }
function showAt(i){ STACK = STACK.slice(0, i + 1); render(); }
function render(){
  document.querySelectorAll('.subview, #view-overview').forEach(function(v){ v.style.display = 'none'; });
  var cur = current();
  var el = cur === 'overview' ? document.getElementById('view-overview') : document.getElementById('view-' + cur);
  if (el) el.style.display = 'block';
  var st = document.getElementById('stage-' + cur);
  if (st && st._needFit) st.fit();
  var html = STACK.map(function(k, i){
    var n = k === 'overview' ? '总览' : (SUB_NAMES[k] || k);
    return i === STACK.length - 1 ? '<b>' + n + '</b>' : '<a onclick="showAt(' + i + ')">' + n + '</a>';
  }).join(' / ');
  document.getElementById('crumb').innerHTML = html;
  document.getElementById('btn-back').style.display = STACK.length > 1 ? '' : 'none';
}
document.addEventListener('keydown', function(e){
  if (e.key === 'Escape') {
    if (document.getElementById('drawer').classList.contains('open')) { closeNote(); } else { showOverview(); }
  }
});
document.querySelectorAll('.cbox').forEach(function(r){
  r.addEventListener('click', function(){
    var tgt = r.getAttribute('data-target');
    if (tgt) { showView(tgt); return; }
    var src = r.getAttribute('data-src');
    if (src) { window.open('https://elixir.bootlin.com/linux/latest/source/' + src, '_blank'); return; }
    openNote(r.getAttribute('data-note'));
  });
});
document.querySelectorAll('.note-btn').forEach(function(b){
  b.addEventListener('click', function(e){ e.stopPropagation(); openNote(b.getAttribute('data-note')); });
});
var NOTES = %NOTES%;
function openNote(key){
  var el = document.querySelector('[data-note="' + key + '"]');
  var n = NOTES[key];
  var name = el ? el.getAttribute('data-name') : (n && n.title ? n.title : key);
  var det = el ? el.getAttribute('data-detail') : '';
  var code = el ? el.getAttribute('data-code') : '';
  var sym = el ? el.getAttribute('data-sym') : '';
  var html = '';
  if (det) html += '<div class="ndet">' + det + '</div>';
  if (code) html += '<pre class="ncode">' + code + '</pre>';
  if (sym) html += '<a class="nlink" target="_blank" href="https://elixir.bootlin.com/linux/latest/C/ident/' + sym + '">📌 定位符号定义（Elixir 交叉引用 · 直达定义行）</a>';
  if (n && n.text) html += '<div class="ntext">' + n.text + '</div>';
  if (n && n.links) html += '<div class="nsec">📖 精选资源</div>' + n.links.map(function(l){
    return '<a class="nlink" target="_blank" href="' + l[1] + '">' + l[0] + '</a>';
  }).join('');
  var kw = encodeURIComponent((name || key) + ' Linux 内核');
  html += '<div class="nsec">🔎 更多资料（自动检索）</div>';
  html += '<a class="nlink" target="_blank" href="https://cn.bing.com/search?q=' + kw + '">🔍 Bing 搜索文章</a>';
  html += '<a class="nlink" target="_blank" href="https://search.bilibili.com/all?keyword=' + kw + '">🎬 B站 视频教程</a>';
  html += '<a class="nlink" target="_blank" href="https://docs.kernel.org/search.html?q=' + encodeURIComponent(name || key) + '">📘 内核官方文档</a>';
  document.getElementById('d-title').textContent = name;
  document.getElementById('d-body').innerHTML = html;
  document.getElementById('drawer').classList.add('open');
}
function closeNote(){ document.getElementById('drawer').classList.remove('open'); }
"""

JS_TAIL = r"""
var STAGE_SIZES = %SIZES%;
Object.keys(STAGE_SIZES).forEach(function(k){
  var s = STAGE_SIZES[k];
  attachZoom('stage-' + k, s[0], s[1]);
});
render();
"""

legend_html = "".join(f'<span class="chip" onclick="showSub(\'{k}\')"><i style="background:{GROUP_COLORS[k][0]}"></i>{n}</span>' for k, n in LEGEND)
views = [f'<div id="view-overview"><div class="stage" id="stage-overview"><div class="inner" id="inner-overview">\n{OVERVIEW_SVG}\n</div></div></div>']
for key, (title, cols) in {**SUBS, **DEEP}.items():
    svg = sub_svg(key, title, cols)
    views.append(f"<div class='subview' id='view-{key}' style='display:none'><div class='stage' id='stage-{key}'><div class='inner' id='inner-{key}'>\n{svg}\n</div></div></div>")
for key, (title, cols) in {**FOUND, **FDEEP}.items():
    svg = sub_svg(key, title, cols, found=True)
    views.append(f"<div class='subview' id='view-{key}' style='display:none'><div class='stage' id='stage-{key}'><div class='inner' id='inner-{key}'>\n{svg}\n</div></div></div>")
views.append(f"<div class='subview' id='view-foundation' style='display:none'><div class='stage' id='stage-foundation'><div class='inner' id='inner-foundation'>\n{foundation_svg()}\n</div></div></div>")
views.append(f"<div class='subview' id='view-roadmap' style='display:none'><div class='stage' id='stage-roadmap'><div class='inner' id='inner-roadmap'>\n{roadmap_svg()}\n</div></div></div>")
views.append(f"<div class='subview' id='view-resources' style='display:none'><div class='stage' id='stage-resources'><div class='inner' id='inner-resources'>\n{resources_svg()}\n</div></div></div>")

js = JS.replace("%SUBNAMES%", str(SUB_NAMES))
js = js.replace("%NOTES%", json.dumps(NOTES, ensure_ascii=False))
html_doc = f"""<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<title>Linux 内核知识结构图（全量版）</title>
<style>{CSS}</style>
</head>
<body>
<header>
  <span class="logo">🐧 Kernel Map</span>
  <span class="crumb" id="crumb"><b>总览</b></span>
  <span class="spacer"></span>
  <button class="btn gold" onclick="showView('roadmap')">🚀 学习路线</button>
  <button class="btn gold" onclick="showView('resources')">📚 资源误区</button>
  <button class="btn gold" onclick="showView('foundation')">🧱 C 地基</button>
  <button class="btn gold" onclick="window.open('https://elixir.bootlin.com/linux/latest/source/','_blank')">📖 Elixir 源码</button>
  <button class="btn primary" id="btn-back" onclick="back()" style="display:none">← 返回</button>
  <button class="btn" onclick="document.getElementById('stage-' + current()).zoomBy(1.3)">＋</button>
  <button class="btn" onclick="document.getElementById('stage-' + current()).zoomBy(1/1.3)">－</button>
  <button class="btn" onclick="document.getElementById('stage-' + current()).setZoom(1)">100%</button>
  <button class="btn" onclick="document.getElementById('stage-' + current()).fit()">适应</button>
</header>
<main>
  <div class="hero">
    <h1>Linux 内核知识结构图</h1>
    <p>点击框图进入子图 · 📄 <b style="color:#8ce0ac">绿色=源码</b> · 🔗 <b style="color:#7ab8ff">蓝色=专题</b> · ⓘ <b style="color:#7ab8ff">精讲资源</b> · 普通条目点击弹讲解 · Esc 返回</p>
    <div class="stats">
      <span class="stat">子系统图 <b>13</b></span>
      <span class="stat">专题深挖 <b>10</b></span>
      <span class="stat">地基图 <b>11</b></span>
      <span class="stat">地基专题 <b>5</b></span>
      <span class="stat">路线图+资源 <b>2</b></span>
    </div>
    <div class="legend">{legend_html}
      <span class="chip" onclick="showView('roadmap')" style="border-color:#8a6d1f;color:#f0d98c">🚀 学习路线</span>
      <span class="chip" onclick="showView('foundation')" style="border-color:#8a6d1f;color:#f0d98c">🧱 C 地基</span>
      <span class="chip" onclick="showView('resources')" style="border-color:#6c3483;color:#cf9ff2">📚 资源误区</span>
      <span class="chip" onclick="openNote('n_bili')" style="border-color:#e0457b;color:#ff9ec4">🎬 视频资源</span>
    </div>
  </div>
  <div class="card">
    {''.join(views)}
  </div>
  <footer>Linux 内核知识结构 · 全量版 v4 · 43 视图 · 点击条目弹出精讲与资源 · 配套文档：linux_kernel_structure.md</footer>
</main>
<div id="drawer" class="drawer">
  <div class="drawer-head"><b id="d-title">讲解</b><button onclick="closeNote()">✕</button></div>
  <div id="d-body"></div>
</div>
<script>
{js}
</script>
<script>
{JS_TAIL}
</script>
</body>
</html>
"""

sizes = {"overview": [W, H_], "roadmap": [], "resources": [], "foundation": []}
r_svg = roadmap_svg()
sizes["roadmap"] = [int(r_svg.split('width="')[1].split('"')[0]), int(r_svg.split('height="')[1].split('"')[0])]
res_svg = resources_svg()
sizes["resources"] = [int(res_svg.split('width="')[1].split('"')[0]), int(res_svg.split('height="')[1].split('"')[0])]
f_svg = foundation_svg()
sizes["foundation"] = [int(f_svg.split('width="')[1].split('"')[0]), int(f_svg.split('height="')[1].split('"')[0])]
for key, v in {**SUBS, **DEEP}.items():
    svg = sub_svg(key, *v)
    sizes[key] = [int(svg.split('width="')[1].split('"')[0]), int(svg.split('height="')[1].split('"')[0])]
for key, v in {**FOUND, **FDEEP}.items():
    svg = sub_svg(key, *v, found=True)
    sizes[key] = [int(svg.split('width="')[1].split('"')[0]), int(svg.split('height="')[1].split('"')[0])]
html_doc = html_doc.replace("%SIZES%", str(sizes))

out = r"D:\GIT-SPACE\D00\_notes\linux_kernel_structure_interactive.html"
with open(out, "w", encoding="utf-8") as f:
    f.write(html_doc)
print("html:", out)
print("subs:", len(SUBS), "deep:", len(DEEP), "roadmap:", len(ROADMAP), "stages")
