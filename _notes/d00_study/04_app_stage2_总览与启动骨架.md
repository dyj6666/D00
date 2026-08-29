# D00 源码研学 · 笔记 04 —— APP 总览：启动骨架与事件驱动架构

> 精读对象：`Core/Src/main.c`(230) · `freertos.c`(298) · `SystemServices/event_bus.c`(177) + `.h` ·
> `module.c`(126) · `Config/app_config.h`(107) · `mem_map.h`(47)
> 阶段 2 开始：APP 共 **~45,000 行自研**（SystemServices 25,290 + Application 10,502 + BSP 6,012 + Core 3,080 + LWIP 171）

---

## 一、APP 全景架构地图

```
APP/APP/
├── Core/            CubeMX 框架：main + freertos + usart + tim + fsmc + rtc + iwdg + it  (3,080)
├── Config/          配置层：app_config(分区/总线/HOSTLINK) + mem_map(外部SRAM) + pinout + var_ids + can_proto
├── SystemServices/  服务层：事件总线/模块表/日志/Shell/HOSTLINK 协议栈/OTA代理/存储/信号源  (25,290)
├── Application/     应用层：LVGL GUI + 网络服务(HTTP/MQTT/SNTP/DNS/ICMP/TCP) + OTA 传输 + 控制算法  (10,502)
├── BSP/             驱动层：LCD/触摸/音频/MPU6050/EEPROM/W25Q128/SRAM/CAN/电源/UART  (6,012)
└── LWIP/App/        lwip.c：ETH 初始化适配  (171)
```

**依赖方向**（严格单向）：Application → SystemServices → BSP → HAL/CubeMX

## 二、APP 启动链（main.c:83-171）

```
复位（BOOT 跳转，MSP 已由裸跳板设置）
  ├─ SCB->VTOR = 0x08010000       （RUN 区向量表重定位；DEBUG_APP=1 时用 0x08000000）
  ├─ __enable_irq()               ★ 关键：BOOT 关闭了全局中断，这里必须重新开启
  ├─ BSP_Watchdog_Refresh()       （发布构建先喂一次狗）
  ├─ HAL_Init / SystemClock_Config（168MHz，同 BOOT）
  ├─ MX_GPIO / DMA / RTC / IWDG / USART1 / TIM2 / TIM3 / FSMC / TIM8 / TIM1 /
  │  USART3 / UART5 / I2C1(MPU6050+AT24C02) / I2C2
  ├─ EventBus_Init()              ★ 调度器启动前初始化事件总线（静态池+双队列）
  ├─ global_tx/rx_stream          （FreeRTOS StreamBuffer：日志 TX 2048B / RX 1024B）
  ├─ osKernelInitialize → MX_FREERTOS_Init（建 4 任务）→ osKernelStart
  └─ while(1) 空转                 （一切交给 RTOS）
```

**外设矩阵速查**：
| 外设 | 用途 |
| --- | --- |
| USART1 (PA9/10) | **HOSTLINK 上位机链路**（DMA 全双工，见 DataLink） |
| USART3 | 调试/Shell 控制台（物理 CH340） |
| UART5 | 摄像头链路（OpenART mini, 115200）→ cam_link |
| TIM1/2/3/8 | PWM/输入捕获等（TIM8_CH1 让位 I2S2_MCK 音频） |
| **TIM7** | **HAL 时间基准**（HAL_TIM_PeriodElapsedCallback → HAL_IncTick，替代 SysTick 让给 RTOS） |
| FSMC NE3 | 外部 SRAM 1MB（IS62WV51216，LVGL/LA 采样） |
| I2C1 | MPU6050 + AT24C02 EEPROM |
| I2C2 | 备用 EEPROM/外设扩展 |
| SPI1 | 外部 W25Q128（与 BOOT 共用同一颗） |
| I2S2+ES8388 | 音频（喇叭） |

## 三、RTOS 任务表（freertos.c）

| 任务 | 优先级 | 栈 | 职责 |
| --- | --- | --- | --- |
| startupTask | High | **4KB**（LWIP 调用链深） | MX_LWIP_Init → LOG_Init → 横幅 → **modules_init()** → 建 1s/200ms 节拍定时器 → 自杀 |
| shellTask | Normal | 4KB | ShellTaskFunction（命令解析，命令层大缓冲） |
| loggerTXTask | High | 512B | LoggerTXTaskFunction（日志流 TX DMA） |
| eventBusTask | **Realtime** | 1.5KB | EventBusTaskFunction（消息分发 + 心跳喂狗） |

**节拍体系**（freertos.c:96-103）：CMSIS-RTOS2 软定时器 `tmr_1s/tmr_200ms` → 回调只发事件 `MSG_SEND_SIMPLE(MODULE_TIMER, MSG_TICK_1S/200MS)`——**定时器回调不做业务，只发事件，业务在订阅者任务上下文执行**（ISR 安全 + 无锁）。

**运行时统计**（:120-132）：DWT->CYCCNT 周期计数器（168MHz）作为 FreeRTOS run-time stats 时钟，32 位回绕由差值运算自然处理。

**栈溢出钩子**（:136-141）：`vApplicationStackOverflowHook → ERR_HandleStackOverflow`（完整转储 + BKP 持久化 + 软复位，与 BOOT 崩溃自愈同哲学）。

## 四、⭐ 事件总线（event_bus.c 177 行）——模块间解耦的唯一通道

**核心流程**：`AllocMsg(静态池) → 填充 → Publish(主队列) → eventBusTask 取出 → dispatch(订阅者表) → FreeMsg(归还池)`

```
                 ┌───────────── 静态消息池 32 槽 × (8B头+128B payload) ─────────────┐
                 │            __attribute__((section(".ccmram"))) ★ 放 CCM！          │
                 └──────────────┬─────────────────────────────────────────┘
                                │ 空闲队列 free_queue (ISR 安全)
              AllocMsg ◄────────┘          Publish/PublishFromISR
      任务/ISR 上下文 ────────────────► 主队列 msg_queue (64 深，存槽指针)
                                                │
                                       eventBusTask (Realtime)
                                                ▼
                                    subs[type] 订阅者表（每类型 ≤8 订阅者）
                                    ──► 逐个调用 handler(msg) ──► FreeMsg 归还
```

**五个设计精髓**：
1. **静态池放 CCM（128KB 紧耦合内存）**——主 SRAM 让给 DMA/ETH 大缓冲；AC5 兼容：槽位用定长结构 `msg_slot_t`（柔性数组不能作数组元素），对外按 `message_t*` 访问
2. **双队列免锁**：空闲池 `xQueueSendFromISR`（池只进不出不会满）+ 主队列——任务/ISR 双上下文发布都安全
3. **丢消息计数**：池空/队列满时 `msg_lost_inc()`（关中断读-改-写，临界区极短），可诊断"总线拥塞"
4. **所有权转移语义**：`EventBus_Publish` 无论成败，调用后 msg 不再归调用方（成功分发后回收，失败立即回收）——杜绝泄漏/双释放
5. **心跳防静默**：eventBusTask 带 1s 超时接收，空闲也周期性 `WDOG_Kick`，配合任务级看门狗

**消息类型**：`msg_types.h` 定义 MSG_* 枚举 + MODULE_* 源 ID（下一课读）。便捷宏 `MSG_SEND_SIMPLE / MSG_SEND_DATA`。

## 五、⭐ 模块注册表（module.c）——33 个模块按优先级自动装配

```c
MODULE_INIT("VAR",     0, VAR_Init),         // 变量管理器最先
MODULE_INIT("Power",   1, BSP_Power_Init),
MODULE_INIT("Cmd",     2, Cmd_Init),
MODULE_INIT("Shell",   4, Shell_Init),
MODULE_INIT("DataLink",10, DataLink_Init),   // HOSTLINK 协议栈
MODULE_INIT("LA_Buffer",20, ...),  LA_Sample 30,  KeyApp 40,  Buzzer 42,  CamLink 44,
MODULE_INIT("TouchSvc",45,...),  LedApp 50,  ImuSvc 52,  GuiApp 55,  OtaAgent 60,
MODULE_INIT("EthApp",  65,...),  Icmp 66, Tcp 67, Mqtt 68, Http 69, SysMon 70,
MODULE_INIT("OtaTcp",  71,...),  OtaCan 72, Audio 75(★), DataAgent 80
```

- **稳定插入排序**（:92-107）：按 priority 升序——依赖模块优先级更低，保证先初始化
- **初始化失败不中断后续模块**（:116-124，逐个 init 不检查返回值）
- **启动堆峰值编排**（:59-61，血泪注释）：Audio prio=75 排在 OtaTcp 之后——启动时 OTA/HTTP server alloc 优先成功，音频任务随后创建（曾因 prio=6 抢堆导致 server alloc 失败）
- **模块表即文档**：启动日志打印注册表（名称+优先级）供核验

## 六、配置层（app_config.h + mem_map.h）

**分区一致性约束**（app_config.h:24-45）：与 BOOT/boot_config.h **严格一致**，改动必须同步两侧：
- OTA 下载槽 1MB（外部 ota_dl）；会话槽 0x080E2000（1024 槽 × 32B，`'OTAM'` magic，断点续传）
- 参数区 0x080E0000 双份 +1024；APP 版本 @0x080DFFFC
- `OTA_CHUNK_MAX 240`：HOSTLINK 单包最大数据块

**事件总线参数**：池 32 槽 / 队列 64 / payload 128B / 每类型订阅者 8。

**HOSTLINK 参数**（:82-90）：CRC-16/MODBUS（poly 0xA001）、RX DMA 256B、TX 帧队列 8（保帧边界）、单帧最大 256B、命令队列 16、**最大注册变量 64 / 订阅 16 / 默认采样周期 10ms**——这些数字是 DataLink 协议栈的契约（下一课核心）。

**外部 SRAM 布局**（mem_map.h，编译期 #error 重叠断言）：
```
0x68000000  LA 采样区 512KB（DMA 流 + 时间戳模式共用）
0x680A0000  LA 预触发环形缓冲 6KB（1024 点 × 6B）
0x680A2000  ExtMem 统一内存池 376KB（LVGL 对象/图像缓存/大缓冲，ext_mem.c 管理）
0x68100000  1MB 顶
```

## 七、设计亮点汇总

1. **事件驱动 + 模块化**：模块间零直接调用，全走事件总线——新增模块 = 注册表一行 + 订阅消息
2. **CCM 妙用**：消息池放 CCM，128KB 紧耦合内存物尽其用，主 SRAM 让给 DMA/ETH
3. **定时器只发事件不做业务**：软定时器回调是 ISR 上下文，绝不阻塞
4. **启动堆峰值编排**：音频模块优先级 75 的刻意安排（血泪注释）
5. **时间基准分离**：TIM7 供 HAL，SysTick 归 RTOS，互不干扰
6. **编译期内存断言**：mem_map 改动越界/重叠立即 #error
7. **双工具链一致**：AC5/GCC 双编译器兼容（__asm 与 naked、定长槽结构规避柔性数组）

## 八、待读清单（下一课）

- [ ] `msg_types.h`：MSG_* / MODULE_* 全枚举（总线契约）
- [ ] `SystemServices/data_link.c`(383) + protocol.c：**HOSTLINK 协议栈**（阶段 2 核心）
- [ ] `SystemServices/logger.c` + shell.c：日志流与 Shell
- [ ] `SystemServices/watchdog.c` + err_mgr.c(562)：任务级看门狗与崩溃管理
- [ ] `SystemServices/var_manager.c` + var_list.c：变量注册表（HOSTLINK 数据源）
- [ ] `SystemServices/ota_agent.c`(522)：OTA 代理（与 BOOT 流水线衔接）

## 九、自测题（读完笔记后自答）

1. 为什么 `__enable_irq()` 在 VTOR 之后必须调用？（提示：BOOT 跳转前状态）
2. 事件总线消息池为什么放 CCM？放主 SRAM 会有什么问题？
3. `EventBus_Publish` 失败时谁负责回收消息？为什么调用方不用管？
4. 定时器回调为什么只发事件不做业务？直接调用业务函数会怎样？
5. Audio 模块优先级为什么是 75？注释里记录的教训是什么？
6. TIM7 与 SysTick 的分工是什么？为什么不用 SysTick 给 HAL 计时？
7. mem_map.h 的三条 #error 分别防什么？
8. HOSTLINK 的 64/16/10ms 三个数字分别是什么契约？
