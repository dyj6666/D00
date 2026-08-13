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
| TcpSvc (server) | 1024B | TCP 控制台监听任务 |
| TcpClient | 2048B | 命令处理(LOG_Printf+netconn_write)栈深 >1KB，实测 help 曾溢出 |
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

## 任务优先级总表（2026-08 更新）

> 统一 CMSIS-RTOS2 优先级枚举；数值 = osPriority 值（越高越优先）。

| 优先级 | 任务 | 职责 / 为什么在此档 |
| --- | --- | --- |
| 48 (Realtime) | eventBusTask | 事件分发中枢：最高，保证事件不被拖延 |
| 40 (High) | loggerTXTask | 日志 DMA 搬运：高优先防日志积压 |
| 32 (AboveNormal) | ImuSvc / TouchSvc / EthIf / tcpip_thread | 传感器实时采样 / 网包入栈 / lwIP 协议栈（高于 netconn 使用者） |
| 24 (Normal) | shellTask / TcpSvc / OtaTcpSvc / LcdUI / DataAgent | 交互/网络服务/UI：同级时间片轮转 |
| 16 (BelowNormal) | HttpSvc / SntpSvc / EthLink | 低优先级周期/后台服务 |
| 9 (Low2) | canRx | CAN 接收分发：高于 TX，防 FIFO 溢出 |
| 8 (Low1) | canTx / DL_CMD / DL_TX / WDOG | CAN 发送 / HOSTLINK 搬运 / 任务看门狗 |
| 2 (BelowLow) | Tmr Svc | FreeRTOS 软件定时器守护 |
| 0 | IDLE | 空闲（WFI 省电钩子） |

规则补充：
1. 任务创建统一 `osThreadNew`（CMSIS-RTOS2）；队列/信号量/通知用原生
   FreeRTOS（ISR 友好、零包装开销）——见 ARCHITECTURE.md 策略。
2. 新增任务必须登记本表 + 实测栈水位，禁止裸优先级数字。
