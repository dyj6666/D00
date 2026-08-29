# D00 源码研学 · 笔记 06 —— 变量系统 / 日志 / 看门狗 / 崩溃管理

> 精读对象：`var_manager.c`(213) · `var_list.c`(114) · `logger.c`(137) · `watchdog.c`(136) · `err_mgr.c`(621)
> 阶段 2 服务层第 2 批：数据通路（变量）+ 输出通路（日志）+ 双保险（看门狗/崩溃自愈）

---

## 一、变量注册表（var_manager.c）—— HOSTLINK 数据中枢

### 1.1 数据结构
```c
typedef struct {          // VarEntry，16B/条
    uint16_t id;          // 变量 ID（协议层寻址）
    const char *name;     // 名称（≤32B，保证条目编码 ≤37B 任何包都装得下）
    VarType type;         // UINT8 / INT16 / INT32 / FLOAT
    uint8_t  permission;  // 1=可写，0=只读
    void *ptr;            // 指向真实数据（直接内存映射，无拷贝）
} VarEntry;

static VarEntry registry[64]  // 放 CCM！（血泪注释：主 RAM .bss 顶曾越过
    .ccmram zero_init         //   _estack 192B，栈一压即踩 bss——见 ENGINEERING_LOG 13.1）
static uint16_t subscribed[16] // 订阅列表（上限 HOSTLINK_MAX_SUBSCRIBE=16）
```

### 1.2 接口矩阵
| 接口 | 语义 | 备注 |
| --- | --- | --- |
| `VAR_Register(id,name,type,perm,ptr)` | 注册变量 | 直接挂指针，**读写零拷贝** |
| `VAR_Read(id,buf,&len)` | 按类型读 | 互斥锁保护（100ms 超时） |
| `VAR_Write(id,buf,len)` | 按类型写 | 只读变量拒绝（perm==1 才可写），长度钳制 |
| `VAR_Subscribe(id)` | 订阅（周期上报） | 去重；满 16 静默忽略 |
| `VAR_ClearSubscriptions()` | 清空订阅 | CMD_SUBSCRIBE 每次全量重建 |
| `VAR_SendList()` | 枚举全部分片发送 | 见 var_list |
| `VAR_GetSubscribedList()` | 取订阅表 | 供周期上报任务用 |

> ⚠️ 注释里的教训：`VAR_Subscribe` 提前返回必须释放锁（:156）——曾经的死锁修复痕迹。

## 二、变量列表分片（var_list.c）—— 先统计后全量发送

**条目编码**：`id(2) + type(1) + perm(1) + name_len(1) + name` = 5+len 字节。
**分片协议**：payload 前 2B = `total_packets + packet_index`（VAR_LIST_FRAG_LEN）。
**两遍式填包**（:71-101）：第一遍只统计不写缓冲（防容量不足越界），第二遍实际填——**先算后写，绝不越界**。
**边界处理**：单条超长仍占一包（保证进度）；空表也发一包；total 上限 255。

## 三、日志系统（logger.c，137 行）—— 流缓冲 + DMA + 发送任务

```
LOG_Printf ──(vsnprintf 256B 缓冲)──► 路由决策：
   ├─ 有 s_sink 且不在中断(IPSR==0) → sink（Shell 命令输出路由）
   └─ 否则 → global_tx_stream（FreeRTOS StreamBuffer 2048B）
                │
        LoggerTXTaskFunction（High，512B 栈）
        xStreamBufferReceive(128B 块, portMAX_DELAY)
                │
        BSP_UART_TransmitDMA(DBG 口)
                │  ← TX DMA 完成 ISR → vTaskNotifyGiveFromISR
        2s 超时自愈：AbortTransmit + 清残留通知 + s_tx_err++
```

**三个要点**：
1. **DMA 缓冲全局对齐**（:22-23 `aligned(4)`）——搬运硬件要求，否则出错
2. **ISR 上下文永远走原始串口**（:100 `__get_IPSR()==0` 判断）——避免 ISR 内写网络（sink 可能是网络适配器）
3. **vsnprintf 超长钳制**（:93-96）——vsnprintf 返回"应写长度"，超长必须钳到缓冲内，否则栈越界读

**RX 方向**：DBG 口 DMA 空闲断帧 → `logger_rx_isr` → global_rx_stream（1024B）→ Shell 命令输入。

## 四、任务级看门狗（watchdog.c，136 行）—— 与 IWDG 双层防线

```
硬件 IWDG（16.4s 硬窗口，谁都不喂就复位）          ← 第 1 层
任务级 WDOG（500ms 巡检，任务不踢就 ERR 复位）      ← 第 2 层（能定位"哪个任务死了"）
```

- `WDOG_RegisterTask(name, handle, timeout_ms)`：注册（懒初始化——允许任务先于 SysMon 运行）
- `WDOG_Kick(handle)`：任务循环里踢
- wdg_monitor_task：每 500ms 巡检，`silent > timeout` → `ERR_HandleTaskStall(name, silent_ms)`（转储+复位）
- 临界区保护表访问（taskENTER_CRITICAL），表 8 项
- 用例：eventBusTask 注册 5000ms 超时，循环里 WDOG_Kick + 心跳防静默（event_bus.c:154-161）

## 五、⭐ 崩溃管理（err_mgr.c，621 行）—— APP 版"黑匣子"

### 5.1 六种崩溃源
| 源 | 入口 | 触发 |
| --- | --- | --- |
| Fault 家族 | `ERR_HandleFaultEntry` | NMI/HardFault/MemManage/BusFault/UsageFault（汇编入口，同 BOOT） |
| RTOS Assert | `ERR_HandleAssert(line)` | configASSERT 失败 |
| 栈溢出 | `ERR_HandleStackOverflow(task)` | vApplicationStackOverflowHook |
| 任务卡死 | `ERR_HandleTaskStall(name, ms)` | 任务级看门狗 |
| 未处理中断 | `ERR_HandleUnhandledIRQ(irqn)` | 未知 IRQ 兜底 |

### 5.2 处理流水线（以 Fault 为例）
```
汇编入口（捕获 MSP/PSP + EXC_RETURN）
  → 冻结中断（__disable_irq，防二次干扰）
  → 收集现场：r0-r3/r12/lr/pc/psr + CFSR/HFSR + MMFAR/BFAR（按 CFSR bit7/15 选）
  → 任务名捕获：直接读 pxCurrentTCB ★（xTaskGetCurrentTaskHandle 内部 taskENTER_CRITICAL
        在 ISR 非法可死锁——所以裸读 FreeRTOS 内部指针）
  → 启发式栈回溯 err_backtrace：PC/LR 先入，再扫描栈 512B 找 Flash 地址（Thumb bit1）
        且去重——"PC 列表"式调用链（非精确 FP 回溯，但够定位）
  → 裸寄存器 USART3 输出完整转储（先裸停 TX DMA，防 HAL 锁等待卡死）
  → BKP reg 1-15 摘要持久化（'ERR1' + src/seq/pc/lr/addr/cfsr/hfsr/tick/任务名12B/CRC）
  → 快速崩溃防抖 err_rapid_crash_check：10s 内 ≥3 次 → g_locked
  → err_recover：锁定则保持喂狗+LED 快闪等待人工（防复位风暴）；
        否则 LED 闪 3 次 → 软复位
  → 下次启动 ERR_ReportLastCrash() 打印 [CRASH] Previous crash recovered
```

### 5.3 BKP 布局（reg 1-15，与 BOOT 的 16-19 隔离）
| reg | 内容 | reg | 内容 |
| --- | --- | --- | --- |
| 1 | magic 'ERR1' | 9 | tick_ms |
| 2 | src | 10-12 | task_name[0..11] |
| 3 | seq | 13 | 摘要 CRC（12 项） |
| 4-6 | pc / lr / fault_addr | 14 | 快速崩溃计数 |
| 7-8 | cfsr / hfsr | 15 | 上次崩溃 RTC 秒 |

### 5.4 与 BOOT boot_err 的对比（设计演进）
| 维度 | BOOT（无 OS） | APP（FreeRTOS） |
| --- | --- | --- |
| 现场 | 汇编入口捕获 | 同左 + pxCurrentTCB 任务名 |
| 调用链 | 无（打印 PC/LR） | 栈扫描回溯（512B，去重） |
| 输出口 | USART2（日志口） | USART3（调试口，裸寄存器） |
| 防抖 | 无（必须自愈） | **10s 内 3 次 → 锁定等人工**（防复位风暴） |
| 摘要 CRC | 简单 bkp_crc | err_bkp_crc（旋转异或 12 项） |
| 布局 | reg 16-19 | reg 1-15（reg 0 留给 OTA 标志） |

## 六、设计亮点汇总

1. **变量指针直挂**：读写零拷贝，协议层拿到的是真实数据的视图
2. **裸读 pxCurrentTCB**：ISR 中获取任务名的正确姿势（注释明确警告 HAL 封装陷阱）
3. **先算后写**：var_list 两遍式填包 + logger vsnprintf 钳制——两类"防越界"范式
4. **快速崩溃锁定**：10s 3 次 → 保持喂狗等待人工，而非无限复位风暴（比 BOOT 更成熟的自愈策略）
5. **双看门狗分工**：硬件 IWDG 兜底复位，任务 WDOG 定位"谁死了"
6. **BKP 三段隔离**：reg0=OTA / 1-15=APP 崩溃 / 16-19=BOOT 崩溃——互不干扰

## 七、待读清单（下一课）

- [ ] `shell.c` + `cmd_shell.c` + `cmd_catalog.c`(2121)：Shell 命令体系
- [ ] `ota_agent.c`(522)：OTA 代理（与 BOOT 流水线衔接）
- [ ] `sysmon.c` + `bsp_system.c`：系统监控
- [ ] `bsp_uart.c`：BSP_UART 抽象实现（DMA 环形 + IDLE）

## 八、自测题

1. VAR 注册表为什么放 CCM？注释里记录的踩栈事故是什么？
2. `VAR_Subscribe` 里曾经有什么 bug？（提示：提前返回路径）
3. `LOG_Printf` 在中断上下文会走哪条路？为什么不能走 sink？
4. err_backtrace 为什么只认"Flash 地址且 Thumb 位为 1"的值？去重解决了什么？
5. 快速崩溃锁定为什么"保持喂狗"而不是停止喂狗让 IWDG 复位？
6. err_mgr 为什么裸读 `pxCurrentTCB`？调用 `xTaskGetCurrentTaskHandle` 会怎样？
7. BKP 的 reg 0/1-15/16-19 三段分别归谁管？为什么这样划分？
8. 任务级看门狗与硬件 IWDG 的分工差异？WDOG 能解决什么问题而 IWDG 不能？
