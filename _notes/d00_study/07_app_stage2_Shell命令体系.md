# D00 源码研学 · 笔记 07 —— Shell 命令体系与统一命令框架

> 精读对象：`SystemServices/shell.c`(237) · `cmd_shell.c`(246) · `Application/cmd_catalog.c`(2228)
> 阶段 2 服务层第 3 批：人机交互三通道（UART Shell / TCP 控制台 / CAN Shell）共用一套命令目录

---

## 一、三层架构：传输无关的命令核心

```
┌─ 传输适配器层 ─────────────────────────────────────────────┐
│  UART Shell (shell.c)          TCP 控制台 (tcp_svc)          │
│  行编辑/历史/补全/方向键        会话式 (cmd_session_t)          │
│  └── 行字符串 ──┐              └── 行字符串 ──┐               │
└──────────────────┼──────────────────────────┼───────────────┘
                   ▼                          ▼
┌─ 统一命令框架 (cmd_shell.c) ─────────────────────────────────┐
│  Cmd_DispatchLine(line, ctx)                                 │
│   · 互斥锁保护（多传输并发安全）                               │
│   · 输出路由：分发期间 LOG_Printf → 当前适配器 out 回调         │
│   · transport 掩码过滤（如 stream 仅 TCP）                    │
│   · s_active 上下文（当前分发中的适配器）                      │
└──────────────────────────────┬───────────────────────────────┘
                               ▼
┌─ 命令实现层 (cmd_catalog.c, 52 条命令) ──────────────────────┐
│  全部 cmd_xxx(const char *args) —— 传输无关，输出走 LOG_Printf  │
│  新增命令 = 实现函数 + cmd_table 一行                           │
└──────────────────────────────────────────────────────────────┘
```

**关键机制**：
1. **LOG_Printf 路由钩子**（cmd_shell.c:164-172 + logger.c:100）：`Cmd_Init` 时 `LOG_SetSink(cmd_log_sink)`——命令分发期间系统打印全部导向当前适配器（TCP 客户端也能看到命令输出）；非分发（空闲/中断）退回原始串口
2. **互斥 + 上下文切换**（:205-230）：`osMutexAcquire` 保护整个分发；`s_active = ctx` 保存/恢复——TCP 与 UART 并发执行命令不串扰
3. **transport 掩码**：`CMD_TRANSPORT_ALL/UART/TCP/CAN` 位掩码，命令声明可用通道；`ota_rbtest` 仅 UART（危险命令限制本地）、`stream` 仅 TCP
4. **Cmd_ActiveTransport()/Cmd_ActiveUser()**：命令内查询"我在哪个通道/哪个客户端"（如 `net cap on` 自动取 TCP 对端 IP 作 EthLab 捕获目标）

## 二、UART Shell 行编辑（shell.c）

| 能力 | 实现 |
| --- | --- |
| 历史 | 环形 8 条，与最近去重，满则 memmove 丢弃最旧 |
| 方向键 | ESC 序列状态机（0=普通→1=收到ESC→2=收到ESC[→A/B） |
| Tab 补全 | 唯一匹配直接补全；多匹配列出候选（名称+说明） |
| 退格 | `\b \b` 三连击 |
| 清行重绘 | `\r + 空格覆盖 + \r`（**不依赖 ANSI，串口助手兼容**） |
| 执行 | `Cmd_DispatchLine(cmd_line, &ctx)`，ctx.out=shell_uart_out |

**ShellTaskFunction**（:224-237）：**16B 块读**代替逐字节读——一次唤醒处理一簇输入，减少流缓冲唤醒次数（高负载不丢字符）。

## 三、命令全景（cmd_table，52 条）

| 域 | 命令 |
| --- | --- |
| 系统 | help / info / ver / echo / reset / taskstats / sysmon / led |
| OTA | **ota**（enter|status|abort|tcp|http \<url\>）/ ota_rbtest（仅 UART，回滚自测） |
| 逻辑分析仪 | la_start/stop/first/trig/dma_start/dma_stop/dump/dma_stat/info/state/peek（11 条） |
| 信号发生器 | sg_uart_start/stop/hex + sg_spi_start/stop + sg_i2c_start/stop/complex（8 条） |
| 网络 | net（status/ping/ip/gw/cap/udp/dbg）/ tcp / icmp / dhcp / dns / sntp / mqtt / http |
| 外设 | lcd / gui / touch / cam / beep / audio / power / mpu / can / w25q / store / usr |
| 测试 | eb_stress（事件总线压测）/ crash（注入，仅调试构建） |
| TCP 专属 | stream on/off（遥测流） |

**经典命令**：
- `cmd_taskstats`（:732-760）：`uxTaskGetSystemState` 输出任务表（状态/优先级/栈高水位），malloc 临时数组用完即 free
- `cmd_ota`（:770-852）：无参→发 `MSG_CMD_OTA_START` 事件（走事件总线进升级）；`http <ip[:port]>/<path>`→URL 解析（去 scheme、拆 host/port/path）→`OtaHttp_Download`；`arg_match` 精确子命令匹配（防 "statusx" 误匹配）
- `cmd_led`（:723-730）：**命令不发函数调用，发事件**（MSG_CMD_LED）——异步解耦，命令上下文不碰 LED 驱动
- `cmd_net cap on`（:150-192）：TCP 通道自动取对端 IP 作 EthLab 捕获目标

## 四、设计亮点

1. **一套命令三通道复用**：UART/TCP/CAN 共享 cmd_table，输出经路由钩子回适配器
2. **危险命令传输限制**：ota_rbtest 仅 UART——远程 TCP 不能触发回滚自测
3. **命令即事件**：部分命令只发事件总线消息，业务在订阅者上下文执行（led/sysmon/ota）
4. **会话式行缓冲**：Cmd_SessionFeed 按 \n 断行（兼容 \r\n），超长行丢弃重来
5. **命令目录即文档**：help 输出名称+说明+可用通道

## 五、待读清单（下一课）

- [ ] `ota_agent.c`(522)：OTA 代理（会话/断点续传/多传输注册）——与 BOOT 流水线衔接的关键
- [ ] `ext_store.c`(478)：外部 Flash 分区存储（OTA 下载槽/备份）
- [ ] `usr_store.c`(378)：用户存储（EEPROM/Flash）
- [ ] `sysmon.c`(228)：系统监控
- [ ] `bsp_uart.c`：BSP_UART 抽象实现

## 六、自测题

1. TCP 客户端执行 `info` 命令时，输出如何回到 TCP 而不是串口？（提示：路由钩子 + s_active）
2. `ota_rbtest` 为什么只允许 UART 通道？transport 掩码机制如何实现？
3. 两个通道同时执行命令会怎样？互斥锁保护了什么？
4. `cmd_led` 为什么发事件而不是直接调 BSP_LED？好处是什么？
5. shell 历史满 8 条时插入新命令做了什么操作？为什么用 memmove 不用 memcpy？
6. `arg_match` 解决了什么问题？（提示：前缀误匹配）
7. 为什么清行重绘用 `\r+空格+\r` 而不是 ANSI 转义？
