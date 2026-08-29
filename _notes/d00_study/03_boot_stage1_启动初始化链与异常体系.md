# D00 源码研学 · 笔记 03 —— BOOT 启动初始化链与异常体系（BOOT 收官）

> 精读对象：`Core/Src/main.c`(198) · `usart.c`(238) · `rtc.c`(106) · `iwdg.c`(60) ·
> `stm32f4xx_it.c`(308) · `BootServices/my_sha256.c`(103)
> 至此 **BOOT 自研代码 100% 精读完毕**（BootApp 604 + BootServices 4,397 + Core 自研约 900 行）

---

## 一、BOOT 启动初始化链（main.c:70-117）——"最小化 + 延迟"哲学

```
复位 → startup 汇编（拷贝 .data/.bss，调 SystemInit）
  → HAL_Init()            （Systick 1ms 时间基准、Flash 预取/指令缓存、NVIC 分组）
  → SystemClock_Config()  （HSE 8MHz → PLL 336/8/2 → SYSCLK 168MHz；APB1=42M APB2=84M）
  → MX_GPIO_Init()        （LED/蜂鸣器/按键）
  → MX_DMA_Init()         （DMA2_Stream2 预备，供 USART1 RX DMA）
  → MX_USART1_UART_Init() （HOSTLINK 口：DMA 环形接收 + IDLE 中断）
  → MX_RTC_Init()         （LSI 32.768kHz → RTC + BKP 备份域）
  → MX_USART2_UART_Init() （日志口）
  → Boot_ErrReportLast()  （★ 复现上次崩溃原因，RTC/USART2 就绪后）
  → BootApp_Run()         （★ 主流程：看门狗→参数→决策树→跳转/升级）
  → while(1) 喂狗          （升级模式退出的兜底，main.c:108-115）
```

**三个"刻意不初始化"（关键设计）**：
1. **`MX_IWDG_Init()` 被注释**（main.c:97）——CubeMX 默认 64 分频不可用，真正生效的是 `boot_watchdog_start()`（boot_app.c:131-143）：**128 分频（250Hz）+ 4095 ≈ 16.4s 硬窗口**。iwdg.c:30-33 注释警告：若恢复调用必须同步为 128 分频，否则窗口翻倍
2. **USART1 启动即配 DMA 环形 + IDLE 中断**（usart.c:61-62）——但 ymodem_port_init()（ymodem_port.c:18-22）会 `HAL_UART_AbortReceive` + 关 IDLE 切回 RXNE 中断模式——**BOOT/APP 复用 UART1 的干净切换**
3. **不早擦参数扇区**——boot_param.c:14-17 注释：早期擦参数扇区实测 Flash BSY 卡死，统一在进入升级模式后延迟执行

## 二、时钟树要点（main.c:123-163）

| 项 | 值 | 推导 |
| --- | --- | --- |
| HSE | 8MHz 外部晶振 | PLLM=8 → 1MHz 参考 |
| PLLN / PLLP | 336 / 2 | SYSCLK = 8/8×336/2 = **168MHz** |
| FLASH_LATENCY_5 | 5 等待周期 | 168MHz 必须 ≥4WS |
| APB1 / APB2 | /4 = 42M / /2 = 84M | 定时器/串口/SPI 时钟来源 |
| LSI | 32.768kHz | **IWDG 时钟（128 分频→250Hz）+ RTC 时钟** |

> ⚡ 与 ESP Flash 的呼应：esp_flash.c:122-123 SPI1 BR=0 → **42MHz = APB2/2**，正是由本时钟树决定。

## 三、串口矩阵（usart.c）

| 口 | 引脚 | 用途 | 中断优先级 | 接收模式 |
| --- | --- | --- | --- | --- |
| USART1 | PA9/PA10 | HOSTLINK 上位机链路 + YMODEM 下载 | **1**（最高） | 启动 DMA 环形+IDLE；升级模式切 RXNE+FIFO |
| USART2 | PA2/PA3 | BOOT 日志（printf 重定向） | 5 | 中断 |
| USART3 | （APP 侧） | APP 调试/Shell 控制台 | — | APP 阶段再读 |

**printf 重定向**（usart.c:224-235）：`fputc → HAL_UART_Transmit(&huart2)`——BOOT 所有 `printf` 走 USART2，与 APP 日志隔离。
**DMA 参数**（usart.c:122-137）：DMA2_Stream2 / CH4 / 外设→内存 / **CIRCULAR 环形** / 字节对齐 / 高优先级。

## 四、RTC 与 BKP 备份域（rtc.c + boot_app.c:125-127）

**时钟**：RTC 用 LSI（rtc.c:72-73），AsynchPrediv=127 / SynchPrediv=255 → 1Hz 日历。RTC 在此项目**主要充当 BKP 备份寄存器的载体**（掉电保持，无需日历功能）。

**BKP 寄存器分工表（重要！HAL 索引 = 硬件寄存器 - 1）**：

| HAL 索引 | 硬件寄存器 | 用途 | 值 |
| --- | --- | --- | --- |
| 0 | BKP_DR1 | **OTA 升级标志**（APP 主动触发） | `0x5A5A` UPGRADE / `0x0000` NONE |
| 1-15 | BKP_DR2~16 | APP err_mgr 崩溃摘要（APP 阶段读） | — |
| 16-19 | BKP_DR17~20 | BOOT 崩溃摘要（boot_err.c） | magic `'BTR1'`/src+seq/PC/CRC |
| 其他 | — | 保留 | — |

> ⚠️ 笔记 01 曾写 "BKP_DR1==0x5A5A"——**正确**！boot_app.c:125 注释明示 "HAL 第二参数为寄存器索引，BKP_DR1 = 索引 0"，`BKP_READ(0)` 读的就是 BKP_DR1。与 AGENTS.md 完全一致。

## 五、异常/中断体系（stm32f4xx_it.c）——双编译器汇编入口

### 5.1 Fault 处理器：汇编捕获现场（:57-190）
5 个 fault（NMI/HardFault/MemManage/BusFault/UsageFault）全部用**汇编入口**包装，**ARMCC `__asm` 与 GCC `naked+内联汇编` 双版本**（同一项目可 Keil/GCC 双工具链编译）：

```asm
TST  LR, #4          ; 检查 EXC_RETURN bit2：0=MSP 1=PSP
ITE  EQ
MRSEQ R0, MSP        ; R0 = 当前栈指针（异常压栈帧基址）
MRSNE R0, PSP
MOV  R1, LR          ; R1 = EXC_RETURN（可区分线程/处理模式 + FPU）
MOV  R2, #1..5       ; R2 = 错误来源编号（对齐 boot_err_src_t）
BX   Boot_ErrFaultEntry
```

**为什么必须汇编**：进入 C 函数时编译器会重排栈操作，此时 LR/栈帧已不可信；汇编在**异常现场原样**捕获 MSP/PSP 与 EXC_RETURN，C 层才能正确解析压栈帧（r0-r3,r12,lr,pc,xpsr）。

### 5.2 外设中断
| 中断 | 处理 | 说明 |
| --- | --- | --- |
| USART1_IRQHandler | `ymodem_uart_isr_handler()` + `HAL_UART_IRQHandler` | 先收 FIFO 再走 HAL（保持 HAL 状态同步，ymodem_port.c:15） |
| USART2_IRQHandler | `HAL_UART_IRQHandler` | 日志口 |
| DMA2_Stream2_IRQHandler | `HAL_DMA_IRQHandler(&hdma_usart1_rx)` | USART1 RX DMA |
| SysTick_Handler | `HAL_IncTick()` | 1ms 时间基准（YMODEM 超时/ESP Flash 等待依赖） |

## 六、自研 SHA-256（my_sha256.c，103 行，FIPS 180-4 标准实现）

```
sha256(data, len, digest)  ── 单发封装
   ├─ init()    状态装载 8 个初始哈希（0x6a09e667...）
   ├─ update()  流式：64B 分块 transform，count 累计字节数
   └─ final()   填充 0x80 + 64bit 位长（大端）→ 最后一次 transform → 输出
```

- 标准实现：消息调度 W[16..63] = SIG1+…+SIG0 扩展，64 轮压缩，CH/MAJ/EP0/EP1 标准算子
- **用途一**：`AES_KEY = SHA256(UID[12] || salt)`（security.c，一机一密）
- **用途二**：固件包摘要校验（security.c 校验链第 ⑤ 关）

## 七、BOOT 崩溃自愈闭环（串联 boot_err + stm32f4xx_it + main）

```
fault 发生
  → 汇编入口捕获现场（MSP/PSP + EXC_RETURN + 来源编号）
  → Boot_ErrFaultEntry：裸寄存器 UART2 打印诊断报告（CFSR 解码原因 + PC/LR/R0-R3）
  → 中止 USART2 TX DMA（防残留数据污染输出）
  → BKP_DR17~20 写崩溃摘要（magic/src+seq/PC/CRC 自校验）
  → NVIC_SystemReset() 软复位（绝不死循环）
  → 下次启动 main.c:100 Boot_ErrReportLast() 打印 "[BOOT-CRASH] ... recovered"
  → BootApp_Run 继续正常决策（崩溃不锁定，自愈）
```

## 八、BOOT 阶段全景图（三篇笔记总收束）

```
BOOT/BOOT（自研 ~5,900 行，已 100% 精读）
├── Core（启动层）          main 初始化链 → RTC/BKP 备份域 → 看门狗 16.4s → 异常体系 [笔记03]
├── BootApp（业务层）       状态机决策树 → 8 阶段升级流水线 → 裸跳板 [笔记01]
└── BootServices（服务层）
    ├── 安全链  security + my_sha256 + aes + uECC   [笔记01 六点五]
    ├── 传输    ymodem + ymodem_port + fifo          [笔记02 五]
    ├── 存储    esp_flash(外) + flash_if(内)         [笔记02 三/四]
    └── 状态    ota_backup + ota_source + boot_param + boot_err + crc32 [笔记02 六~九]
```

**BOOT 阶段结论**：这是一个"工业级安全引导"——一机一密（UID 派生）、六道安全关（签名/摘要/芯片绑定/防重放/尺寸/版本）、双区双份（内部 RUN + 外部备份、参数双份）、断电自愈（每阶段可恢复）、崩溃自愈（fault→复位→复现）。**任何阶段失败都"宁停勿损"，绝不让坏固件上线。**

## 九、BOOT 阶段自测题（三篇笔记合集，全部自答后再进 APP）

1. 完整画出从复位到跳转 APP 的初始化顺序，并说明每步失败会怎样（笔记03 一）
2. HAL 索引 0 对应硬件哪个 BKP 寄存器？0x5A5A 是谁写的？（笔记03 四）
3. 为什么 fault 入口必须汇编？C 函数入口有什么问题？（笔记03 五）
4. IWDG 为什么是 128 分频 + 4095 ≈ 16.4s？谁配置的？CubeMX 的 64 分频为什么被注释？（笔记03 一/六、boot_app.c:131）
5. USART1 在 BOOT 两种模式下接收方式有何不同？谁负责切换？（笔记02 五.5）
6. 备份头为什么独立 4KB 扇区？三个 CRC32 为什么不统一？（笔记02 二/八）
7. YMODEM 的 CRC32 与标准 YMODEM CRC16 差异，上位机要配套什么？（笔记02 五）
8. 升级到一半断电，复位后哪个状态机分支接管？（笔记01 四 + 笔记02 六）
9. APP 启动成功后如何把 PENDING 清回 NORMAL？（笔记01 三——APP 侧确认，待 APP 阶段验证）

## 十、下一课预告：APP 阶段

- APP/APP 结构：SystemServices（HOSTLINK 协议/服务）/ Application（业务任务）/ BSP（驱动）共 53k 行
- 首读：APP 入口 main → FreeRTOS 启动 → HOSTLINK 协议栈（上位机链路 UART1）
