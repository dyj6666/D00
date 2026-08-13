# 任务栈预算表

> 原则（2026-08 起，见 ENGINEERING_LOG 10.27/10.28）：
> **Keil 实测高水位（HW）不能直接套用 GCC**——不同编译器/printf 实现的
> 栈深差异可达数百字节。任务栈必须以"目标工具链实测 + 50% 余量"为准，
> 且每次改动后重新核对 `taskstats`（剩余 <100 词即预警）。

## 任务清单（当前值）

| 任务 | 栈大小 | 说明 / 实测依据 |
| --- | --- | --- |
| startupTask | 4096B | MX_LWIP_Init 调用链深；kept |
| LcdUI | 2560B | 3D 渲染/格式化余量；GCC 实测余 487 词（曾 1536B 溢出） |
| eventBusTask | 1536B | sysmon 处理栈深；GCC 实测余 312 词（曾仅 144B） |
| ImuSvc | 2048B | GCC 浮点打印 %+.3f；曾 1024B 溢出 |
| TouchSvc | 1024B | GCC 浮点标定；曾 512B 偏薄 |
| OtaTcpSvc | 2048B | Ota_Begin(擦除+会话)+netconn；曾 1024B 溢出 |
| ShellTask | 2048B | 命令解析含协议栈调用 |
| SntpSvc | 1024B | netconn 路径峰值 ~752B（不可再砍，768B 余量仅 16B） |
| TcpSvc / TcpClient | 1024B | TCP 控制台会话 |
| DL_TX / DL_CMD | 1024B | 纯搬运 / 命令解析 |
| LoggerTXTask | 512B | 纯搬运 |
| WDOG | 512B | 轻量监控 |
| SG (signal_gen) | 512B | 信号发生器测试任务 |

## 规则

1. 任务栈改动必须同时验证 Keil 与 GCC（若该工具链用于目标发布）。
2. 以 `taskstats` 实测剩余为准，低于 100 词（400B）即预警。
3. 涉及 printf/浮点/网协栈的任务，余量按最坏路径 +50% 留。
4. 栈分配从 FreeRTOS 堆（CCM，当前 53KB）切出，总量需留 ≥4KB 余量。

## 当前堆余量

- FreeRTOS 堆（CCM 0x10000000）：53KB，启动后空闲 ~13KB。
- lwIP 内存：独立静态池（MEM_SIZE 12KB + pbuf 池），不占 FreeRTOS 堆。
