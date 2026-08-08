# 工程日志 · 问题复盘与架构演进

> 本文档记录本项目从架构评审到工业化改造过程中遇到的**全部问题、根因、解决方案与验证结果**，
> 用于复盘与学习。配套文档：[架构指南](ARCHITECTURE.md)。

| 项目 | 内容 |
| --- | --- |
| 周期 | 2026-07 ～ 2026-08 |
| 芯片/平台 | STM32F407ZGT6 + FreeRTOS V10.3.1 + Keil AC5 / ARM GCC |
| 当前状态 | Keil **0 Error / 0 Warning**；GCC 固件构建通过；主机测试全绿 |

---

## 1. 体检总览

| 维度 | 结论 |
| --- | --- |
| 构建体系 | Keil 与 GCC 双路径均可独立构建并产出 APP.bin |
| 安全边界 | 协议帧长度/CRC/命令均严格校验，无已知越界路径 |
| 平台解耦 | BSP 抽象层就位，服务层无直接 HAL 调用（除 LA 平台模块） |
| 资源管理 | 事件总线静态池、无堆碎片；互斥锁均带超时保护 |
| 可靠性 | IWDG + 任务级看门狗双层防线；SRAM 自检；调试构建模式 |
| 可测试性 | 协议/CRC/分片纯逻辑层有主机单元测试，CI 覆盖主机与固件构建 |

---

## 2. P0 · 安全与协议

### 2.1 DataLink 越界读取（最严重）
- **现象**：上位机帧内 16 位 `payload_len` 被无条件信任，恶意/损坏帧可越界读取 256 字节接收缓冲（最大 65535 字节）。
- **根因**：命令处理直接按帧内长度字段循环，未与实际接收长度比对。
- **解决**：抽出纯逻辑协议层 `protocol.c`（构造/解析/校验），所有命令先经
  `Protocol_ParseHeader` 校验 `payload_len == len - 5`，再按命令做二级边界检查
  （SUBSCRIBE 偶数长度、READ/WRITE_VAR 最小长度与值长度）。
- **验证**：主机测试覆盖构造/篡改/截断/长度不符等 74 项断言。

### 2.2 坏帧导致后续帧丢失
- **解决**：接收端 CRC 失败后在缓冲内滑动重同步（`AA 55` 定位），单帧损坏不再拖垮后续帧。

### 2.3 协议无版本与错误反馈
- **解决**：新增 `CMD_GET_INFO` 版本查询、`CMD_ERROR` 统一错误响应（原命令码 + 错误码）。

---

## 3. P1 · 健壮性

### 3.1 事件总线动态分配造成堆碎片
- **根因**：每条消息 `pvPortMalloc`，高频发布产生碎片且 ISR 内释放不安全（需死信队列兜底）。
- **解决**：改为**静态消息池**（32 槽 × 128B），空闲槽用 FreeRTOS 队列管理，分配/回收 ISR 安全，
  池空时计数丢失，`sysmon` 可查。

### 3.2 LIST_VARS 只发第一包
- **根因**：`VAR_SendList` 循环内无条件 `break`，且分片总数估算不准、存在除零风险。
- **解决**：抽出纯逻辑分片模块 `var_list.c`，先统计再按 `total/packet_index` 全量发送；
  空表也发一包；`VAR_Register` 增加名称长度上限保证单条必能装入。

### 3.3 逻辑分析仪调试残留
- **根因**：`la_samples=10/100` 硬编码调试、静态 `last_state` 不重置、触发状态机（Arm/SetTriggered）
  未接线、缺下降沿触发。
- **解决**：清理残留；触发状态机真正接线（armed → 边沿匹配 → 单次触发 → 自动解除武装）；
  补下降沿；触发深度满足后自动停采并保留预触发数据。

---

## 4. P2 · 硬件与可靠性

### 4.1 “假 DMA”采样（硬件真相）
- **根因**：原方案 TIM3+DMA1 读 GPIO，但 **DMA1 无法访问 AHB1 外设（GPIO->IDR）**，
  TIM3 的 DMA 请求只映射到 DMA1，硬件上不可能，代码实际退化为软件定时器伪 DMA（约 1 kHz）。
- **解决**：改用 **TIM1_UP → DMA2 Stream5 / Channel 6**（已在 stm32f4xx-hal 映射表交叉验证），
  APB2 168 MHz 分频整字搬运 `GPIOB->IDR` 到 32 KB 环形缓冲，采样率可配置。
- **验证**：CubeMX 配置（.ioc）、Keil 工程、启动向量、中断文件同步更新；GCC 构建通过。

### 4.2 看门狗只有硬件层
- **解决**：新增任务级看门狗 `watchdog.c`，EventBus / DataAgent 心跳监控，超时记录后软件复位；
  与 IWDG 形成“任务级 + 硬件级”双层防线。

### 4.3 AC5 编译器兼容问题
- **现象**：`event_bus.c` 用含柔性数组成员（`payload[]`）的类型做数组元素，AC5 报 `#1057`。
- **解决**：槽位改为同布局的定长结构体，对外 `message_t` 接口不变。

### 4.4 触发状态变量“写了没用”
- **现象**：`la_trigger.c` 的 `armed` 从未被读取（AC5 告警 `#550-D`）。
- **解决**：真正用于触发门控并补充 `LA_Trigger_IsArmed()`，触发后自动解除武装。

---

## 5. P3 · 构建与工程化

### 5.1 编码问题实为假象
- **澄清**：业务源码本就是合法 UTF-8 中文，此前“乱码”是 PowerShell 控制台 GBK 代码页所致；
  已用 `.editorconfig` + `.vscode/settings.json` 固化编码规则，并修复 `usart.c` 一行被损坏为
  U+FFFD 的中文注释。

### 5.2 BSP 抽象层
- **动机**：HAL 散落在服务层，跨厂商移植需重写 5+ 文件。
- **解决**：新增 `BSP/`（system / uart / watchdog / rtc / gpio），logger、data_link、sysmon、
  ota_agent、shell、led、key 全部改走 BSP 接口；**服务层与应用层不再直接调用 HAL**。

### 5.3 变量 ID 集中分配
- **解决**：新增 `Config/var_ids.h` 集中登记，模块内禁止写数字，杜绝 ID 冲突。

### 5.4 任务创建方式统一
- **解决**：data_link（TX/CMD）与 watchdog 监控任务由 `xTaskCreate` 迁移到 CMSIS-RTOS2
  `osThreadNew`，与 freertos.c / data_agent 保持一致。

### 5.5 独立于 Keil 的 GCC 固件构建
- **解决**：引入官方 FreeRTOS GCC ARM_CM4F 移植层、APP 链接脚本
  （0x08010000 起，960 KB）、CMake 工具链，`cmake --build` 直接产出 `firmware.elf` + `APP.bin`；
  CI 增加固件交叉编译任务。

### 5.6 模块注册表增强
- **解决**：模块表带名称与优先级，按依赖排序初始化，启动日志可读，删除伪地址日志。

### 5.7 运行时间统计（CPU 使用率）
- **根因**：`configureTimerForRunTimeStats` / `getRunTimeCounterValue` 是空实现，
  `sysmon` 的 CPU 项恒为 0%。
- **解决**：用 DWT 周期计数器（168 MHz）实现两个统计钩子，
  32 位回绕由 FreeRTOS 差值运算自然处理。

### 5.8 命令队列深度与可观测性
- **解决**：HOSTLINK 命令队列由 4 提升到 16（`HOSTLINK_CMD_QUEUE_LEN`），
  溢出时计数并可在 `sysmon` 的 DataLink 项查看，丢帧不再是“静默”问题。

### 5.9 外部 SRAM 上电自检
- **解决**：`LA_Buffer_Init` 做全量写读校验；失败后采样写读自动拒绝，
  `LA_Buffer_IsSramOk()` 可查询，避免 FSMC/SRAM 故障时踩硬错误。

### 5.10 调试构建模式
- **解决**：`APP_DEBUG_MODE` 编译开关，置 1 时关闭 IWDG 与任务级看门狗，
  供 gdb 断点调试；发布构建强制置 0。

### 5.11 服务层零 HAL 收口
- **解决**：shell 的 LA 诊断命令改走 `LA_Diag_PrintExtiStatus()` 与
  `LA_Sample_GetChannelStates()`，服务层/应用层彻底无 HAL/寄存器直读。

### 5.12 协议常量统一
- **解决**：`data_agent.c` 组包改用 `protocol.h` 的 `SYNC1/SYNC2/CMD_DATA`，
  消灭业务层裸常量。

### 5.13 移除死代码 LCD
- **解决**：删除 `Drivers/lcd.c` / `lcd.h` 并从 Keil 与 GCC 构建中摘除
  （此前唯一引用是被注释的 `LCD_Init()`，且 GCC 构建暴露一处格式告警）。

---

## 6. 复盘提炼（原则）

1. **信任边界**：外部输入（帧长、索引、长度字段）必须先与实际缓冲比对再使用。
2. **纯逻辑与平台分离**：协议、CRC、分片等无硬件依赖的代码单独成层，才能主机测试、跨平台复用。
3. **硬件事实优先于“看似可行”**：DMA 能否访问某外设由总线映射决定，动手前先查参考手册/映射表。
4. **ISR 只做入队，业务在任务里**：中断内只允许 `FromISR` API 与快速操作。
5. **资源提前分配**：StreamBuffer / Queue / 消息池在调度器启动前就绪。
6. **双编译器验证**：AC5 与 GCC 的严格程度不同，双路径构建能提前暴露兼容问题。
7. **集中化配置**：引脚、软件参数、变量 ID 各自一个文件，禁止散落硬编码。
8. **可观测性**：事件丢失计数、看门狗静默时长、复位原因，全部可查可打印。

---

## 7. 遗留事项

代码层面问题已全部闭环。剩余为**硬件验证待办**（需实物板）：

| 事项 | 验证方法 |
| --- | --- |
| DWT CPU 统计数值合理性 | `sysmon` 观察各任务 CPU 占比之和接近 100% |
| SRAM 自检通过日志 | 上电日志无 “SRAM self-test failed” |
| TIM1+DMA2 采样 | `la_dma_start 100000` → `la_dma_stop` 检查计数与波形 |
| 触发/预触发链路 | `la_trig` + 信号源验证边沿捕获 |
| 双构建产物一致性 | 分别烧录 Keil 与 GCC 产物做冒烟 |

---

## 8. BOOT 联合检查与分区一致性（2026-08）

### 8.1 GCC 链接脚本范围与 BOOT 分区不符（自研缺陷，已修复）
- **根因**：`Core/Startup/STM32F407ZGTx_APP.ld` 将 FLASH 设为 960 KB、RAM 设为 192 KB，
  而 BOOT 分区约定 APP 仅 256 KB（`0x08010000~0x0804FFFF`）、F407 主 SRAM 仅 128 KB。
  若固件膨胀会静默覆盖 Download 分区与魔数区。
- **解决**：FLASH `0x40000`、RAM `0x20000`，与 `APP.sct`、BOOT 分区三方一致。
- **验证**：GCC 构建后 `_estack=0x20020000`、代码止于 `0x08020F58`、bss 止于 `0x20013F50`。

### 8.2 直烧 APP 无有效性魔数（机制缺口，已补工具）
- **根因**：`0x0804FFF8` 的 APP 有效性魔数（`0x4F54412E`）只在 BOOT 的 OTA 流程末尾写入；
  整片擦除后直接烧录 APP.bin（SWD/Keil）时该处为 `0xFFFFFFFF`，BOOT 判定无效进入升级模式。
- **解决**：新增 `Script/append_app_magic.py`，把原始 APP.bin 补齐为 256 KB 完整分区镜像
  （0xFF 填充 + 魔数 + 版本号），并接入 GCC 构建自动生成 `APP_flash.bin`。
- **验证**：`APP_flash.bin` 大小 262144、`0x3FFF8=0x4F54412E`、`0x3FFFC=version`。
- **用法**：Keil 路径下执行
  `python ..\Script\append_app_magic.py .\Output\APP.bin --version N`。

### 8.3 APP.sct 注释乱码且魔数区未落地
- **解决**：重写 `MDK-ARM/APP/APP.sct`（纯 ASCII 注释），明确魔数/版本由 OTA 或
  `append_app_magic.py` 写入、不参与链接；RW 区声明与 128 KB 主 SRAM 一致。

### 8.4 BOOT 侧问题（详见 BOOT 工程日志）
- BOOT.sct 的 `RW_IRAM1` 曾声明 192 KB（实际 128 KB），已修正。

---

## 9. 上板回归：任务看门狗误复位（已修复）

### 9.1 DataAgent 心跳位置导致无限复位（自研回归）
- **现象**：烧录后约 5.5 s 出现 `[WDOG] Task 'DataAgent' stalled (5490 ms silent) -> reset!`，
  之后无限复位重启。
- **根因**：`WDOG_Kick(self)` 被放在 `if (count == 0) continue;` **之后**——
  无上位机订阅时（`count == 0`）每周期直接跳过踢狗，5 s 后任务级看门狗判定 stall。
- **解决**：将心跳移到循环顶部，**无条件**每周期上报；
  EventBus 同步加固：阻塞接收改为 1 s 超时接收，无论是否收到消息都踢狗。
- **验证**：Keil 全量重编译 0/0、GCC 固件构建通过、`APP_flash.bin` 重新生成；
  上板后应持续运行不再复位。

### 9.2 sysmon CPU 统计在 DWT 回绕后失真（上板发现）
- **现象**：实机 `sysmon` 的 CPU 百分比乱跳（DataAgent 28%~52%、IDLE 20%~65%），
  与任务真实负载严重不符（10 ms 周期任务不可能占 28% CPU）。
- **根因**：DWT 周期计数器在 168 MHz 下 32 位约 **25.6 s 回绕**，FreeRTOS
  “启动以来累加值”溢出后直接求百分比无意义。
- **解决**：`print_cpu_usage` 改为 **1 s 窗口差分算法**（两次快照按任务名求差，
  64 位中间量防乘溢出），窗口远小于回绕周期，数值稳定；
  同时将 `vTaskList` 缓冲 512 → 768，消除任务列表截断。
- **验证**：Keil 0/0、GCC 构建通过、`APP_flash.bin` 重新生成；上板复测通过
  （IDLE 99%、其余任务 0%，数值稳定合理）。

### 9.3 sysmon 任务列表截断 + EventBus 栈余量过低（上板发现）
- **现象**：`sysmon` 的 TASKS 段在约 230 字节处被截断（`taskstats` 正常），
  且 `eventBusTask` 栈余量最低只剩 184 字节。
- **根因**：一次性打印超长字符串（vTaskList 全量输出 + 头部）越过日志缓冲边界；
  新版 `sysmon`（1 s 延时 + CPU 统计）放大了 EventBus 任务的栈压力。
- **解决**：任务列表改为 `uxTaskGetSystemState` **逐行格式化输出**（每行均短，
  彻底绕开长串截断）；`eventBusTask` 栈 2048 → 4096 字节。
- **验证**：Keil 0/0、GCC 构建通过、DAP 烧录 Verify OK；上板复测通过
  （TASKS 9 任务完整、eventBusTask 栈余量 184 → 902）。

---

## 10. 实机测试记录（2026-08，DAP + 双串口联调）

### 10.1 烧录/复位/启动链路
- DAP（CMSIS-DAP）+ Keil `-f` 烧录：Erase / Programming / Verify OK；
- shell `reset` 软复位：BOOT 校验魔数 → 跳转 APP → 9 模块初始化全流程正常；
- 复位原因识别：手动复位=External pin reset。

### 10.2 HOSTLINK 协议全功能回归（COM13, 921600，10/10 通过）
| # | 用例 | 结果 |
| --- | --- | --- |
| 1 | GET_INFO 版本查询 | ✅ 协议 v1 / APP 1.0.0 |
| 2 | READ_VAR 结构 | ✅ 返回 id/len/reserved/value |
| 3 | WRITE_VAR 写 12345 | ✅ 无错误响应 |
| 4 | 回读=12345 | ✅ |
| 5 | 长度字段越界 | ✅ 拒绝并回 BAD_PAYLOAD_LEN |
| 6 | 未知命令 0xFF | ✅ 回 UNKNOWN_CMD 错误帧 |
| 7 | LIST_VARS 非法 payload | ✅ 回 BAD_PAYLOAD_LEN |
| 8 | 坏 CRC | ✅ 丢弃无响应 |
| 9 | 写后持久 | ✅ 仍为 12345 |
| 10 | 恢复 writable=0 | ✅ |

### 10.3 发现与说明
- 协议暂无“取消订阅”命令：SUBSCRIBE 后数据帧持续上报，需复位或重新订阅清零；
  如需可后续新增 `CMD_UNSUBSCRIBE`（当前不影响功能）。

### 10.4 逻辑分析仪全量测试（PB4 ← PC6 100Hz PWM，占空比 ~5%）

**DMA 流模式（TIM1+DMA2，100 kHz）**
| 指标 | 实测 | 理论 | 结果 |
| --- | --- | --- | --- |
| 采样率 | 200112 样本 / 2 s | 100 kHz | ✅ |
| 信号频率 | 100.0 Hz（周期 1000.0 样本） | 100 Hz | ✅ |
| 占空比 | 4.9% | ~5% | ✅ |

**时间戳模式（EXTI 双边沿）**：2 s 采集 704 边沿（≈100 Hz × 2 边沿 × 窗口），
时间戳与状态字正确。

**触发模式（CH4 上升沿 + 自动停采）**：13 s 后自动停采，样本 **2051** =
后触发 2048 + 预触发 3，预触发数据完整保留，触发状态机（armed→fire→disarm→
auto-stop）全部按设计工作。

### 10.5 测试期间修复：大体积日志静默丢帧
- **根因**：`la_dump` 一次性快速输出 18 KB，2 KB 日志流缓冲被灌满后 `LOG_Printf`
  静默丢弃（115200 波特率排空约 11.5 KB/s，生成速度远超排空速度）。
- **解决**：导出命令按行限速（每行 `vTaskDelay(10ms)`），匹配串口吞吐。
- **验证**：2048 样本完整导出（此前仅 337 样本即被截断）。

---

## 11. 逻辑分析仪 MCU 端极致化（2026-08）

### 11.1 新增能力
- **条件触发**：边沿 + 条件通道电平组合（如 I2C START = SDA 下降沿且 SCL 为高）；
  命令 `la_trig <type> <ch> [post] [cond_ch] [cond_level]`。
- **可配置触发深度**：`post` 参数设定触发后自动停采点数。
- **DMA 双缓冲模式**：`la_dma_buf <sram|iram>` 运行时切换
  （IRAM 8192 点 / SRAM 32768 点，采集中禁止切换）。
- **溢出检测**：DMA 错误回调 + overrun 标志，`la_dma_stat` 可查。
- **状态查询**：`la_info`（缓冲/自检/触发配置）、`la_dma_stat`（计数/溢出/缓冲）。
- **全缓冲导出**：`la_dump` 上限提升到当前缓冲深度，按行限速防丢帧。

### 11.2 实测数据
| 模式 | 速率 | 结果 |
| --- | --- | --- |
| IRAM 8192 | 10 MHz | 21.4M 样本/2s，无溢出（≥21 MHz 仍可用） |
| SRAM 32768 | 100 kHz | 深捕获正常，无溢出 |
| SRAM 32768 | ~6 MHz | FSMC 带宽上限（更高速率建议 IRAM） |
| 条件触发正向 | CH4↑ 且 CH2==0 | 2051 样本，触发正常 |
| 条件触发反向 | CH4↑ 且 CH2==1（恒假） | 0 样本，永不触发 ✅ |
| SRAM 波形 | 100Hz PWM | 周期 1000 样本 / 占空比 4.9% ✅ |

---

## 12. 逻辑分析仪上位机配套（2026-08）

### 12.1 新增 `CMD_LA_DUMP` 二进制高速导出
- **动机**：shell 文本导出（115200）太慢，无法支撑上位机实时分析。
- **实现**：HOSTLINK 新增 `CMD_LA_DUMP(0x07)`，请求 `offset(u32)+count(u32)`，
  响应分帧回传原始 32 位采样（每帧 28 样本，帧间限速防 TX 流缓冲溢出）。
- **验证**：实机 921600 下载 1024 样本，PWM 模式正确（占空比 4.9%）。

### 12.2 上位机 LogicAnalyzer Pro
- 位置：`HOST/LogicAnalyzer/`，PyQt5 + pyqtgraph + numpy。
- 功能：采集控制（shell）+ 二进制下载（HOSTLINK）、8 通道波形/缩放/光标/区间测量、
  UART/I2C/SPI 解码器（纯逻辑可单测）、位视图（二进制/HEX/十进制/ASCII）、CSV 导出。
- 验证：解码器单元测试全绿（UART 0x55 / I2C START-ADDR-DATA-STOP / SPI MOSI·MISO）；
  UI 离屏加载 + 波形冒烟通过。

### 12.3 采样通道重构：8 → 4 独立通道（PG6/PG7/PG12/PG15）
- **动机**：探索者V3 板 PB1~PB3/PB5~PB7 在 PCB 上与外部 SRAM 总线直连，8 通道方案中
  除 PB0/PB4 外均为总线伪信号；按引脚分配表（E 列"完全独立"=Y）核验后
  改用 Port G 的 4 个空闲独立引脚，彻底消除伪通道。
- **固件实现**：
  - `la_config.h` 通道映射集中配置：`LA_GPIO_PORT=GPIOG` +
    `LA_CHANNEL_PINS={GPIO_PIN_6,7,12,15,0,0,0,0}`，改一处即可换引脚；
  - 采样打包与 DMA 导出按映射归一化（数据位 i = 通道 i），DMA 源改为 `LA_GPIO_PORT->IDR`；
  - 时间戳 EXTI 迁至 PG 组：`gpio.c` 只保留 PG6/7/12/15 的 EXTI 配置与
    EXTI9_5/EXTI15_10 中断；删除 PB0~PB7 的 EXTI0-4 中断处理，
    新增 `EXTI15_10_IRQHandler`；
  - `HAL_GPIO_EXTI_Callback` 由引脚范围限制改为 `la_pin_to_channel()` 查表过滤；
  - `la_ch4_state` → `la_ch3_state`（变量名同步，ID `0x7003` 不变，协议兼容）；
  - shell 诊断：`la_read_pb4` → `la_state`（打印 CH0~3 电平），`la_peek` 同步更新。
- **上位机适配**：控制面板新增"通道数"（4/8，默认 4）；波形 y 布局、解码通道范围、
  CSV 导出列数均随 `TraceData.nchannels` 动态适配。
- **CubeMX 注意**：`.ioc` 中 PB0~PB7 的 EXTI 配置尚未迁移（手改 `Mcu.PinNN` 编号
  不可靠，易破坏 CubeMX 工程）。下次用 CubeMX 打开工程时，删除 PB0~PB7 的
  `GPXTI0~7` 信号与 NVIC EXTI0-4，改配 PG6/PG7/PG12/PG15 的
  `GPXTI6/7/12/15`（NVIC 保留 EXTI9_5 并勾选 EXTI15_10），再重新生成
  `gpio.c`/`stm32f4xx_it.c` 即可与当前手改结果一致。
- **验证**：Keil AC5 0 Error / 0 Warning；GCC（arm-none-eabi 13.3.1）构建通过并产出
  `APP_flash.bin`；解码器/协议单元测试全绿。
- **实机验证**（PC6 输出 100Hz/5% PWM → PG6）：
  - `la_state` 循环采样确认 CH0 存在跳变（40 次采样 1 次高，≈占空比 5%）；
  - 上位机 100 kHz 抓取 8192 点：CH0 = **100.0 Hz / 4.9% 占空比 / 8 个上升沿**，
    与信号源完全吻合；
  - CH1~3 恒为板载空闲高电平（无线 CE/CS、LCD CS、摄像头复位相关电路默认电平），
    无任何总线伪信号，4 通道映射与归一化正确。

### 12.4 UART 信号发生器 hex 模式与复杂帧验证
- **动机**：文本模式（HELLO）只覆盖可打印 ASCII，无法验证 0x00/0xFF/0x80 等
  边界值与二进制协议帧；新增 `sg_uart_hex` 命令按十六进制字符串发送任意字节帧。
- **实现**：`signal_gen.c` 重构为字节缓冲（`sg_data`+`sg_len`），
  `SG_UartStartHex()` 解析偶数长度 hex 串（非法字符返回 -1）；
  shell 新增 `sg_uart_hex <baud> <hexstr> [interval_ms]`。
- **实机验证**（115200，帧 `AA 55 08 00 FF 7F 80 0D 0A 5A 4B`，11 字节 × 6 帧）：
  - 解码 66 字节：帧内字节逐位匹配，0x00/0xFF/0x7F/0x80/CR/LF 全部正确；
  - 位标注（S/Dn/STOP/HEX+ASCII）与解码结果一致；
  - 上位机波形协议位标注功能（字节级 HEX+ASCII 汇总 + 位级 S/D/STOP）同步验证通过。

### 12.5 SPI 信号发生器与多字节帧解码修复
- **动机**：验证 SPI 协议解析；硬件上 SPI2（PB13=SCK/PB15=MOSI/PB12=CS）未被占用，
  接线 PB13→PG6(CH0)、PB15→PG7(CH1)、PB12→PG12(CH2)。
- **实现**：
  - `signal_gen.c` 新增 SPI2 主机（模式0、164kHz、MSB、CS 低有效）后台任务，
    shell 命令 `sg_spi_start <hexstr> [interval_ms]` / `sg_spi_stop`；
  - 启用 `HAL_SPI_MODULE_ENABLED`，Keil/GCC 构建均加入 `stm32f4xx_hal_spi.c`。
- **解码器修复**：`decode_spi` 解完一字节后把 i 推进到 CS 拉高，导致多字节 SPI 帧
  只解出第一字节；改为在帧内自然续解。单测回归全绿。
- **采样率要求**：SPI 164kHz 在 1MHz 采样（每半周期 3 样本）下边沿检测漏沿，
  需 5MHz 采样（每 bit 30 样本）方可稳定解码——上位机采样率选 5MHz。
- **实机验证**（5MHz 采样，帧 `A5 3C 55 AA FF 00`，2ms 间隔）：
  窗口内 3 帧 × 6 字节全部正确，CS 帧长一致（1493 样本），无错位。

### 12.6 I2C 信号发生器与位级标注（含鲁棒性多组验证）
- **动机**：验证 I2C 协议解析；探索者 V3 的 PB10/PB11 未引出且 I2C1(PB8/PB9)
  接板载 24C02，改用**软件模拟 I2C（bit-bang）**：PE2=SCL / PE3=SDA，DWT 微秒
  延时，100kHz，高优先级任务避免被抢占。
- **实现**：
  - `sg_i2c_start <addr> <hex>` 简单写帧（模拟从机 ACK）；
  - `sg_i2c_complex <addr>` 复杂演示帧：写 3 字节+ACK → 重复起始 → 读地址+ACK
    → 最后字节 NACK → STOP，覆盖 START/重复START/地址/读写位/应答/NACK/STOP；
  - 修复 SDA 翻转未稳定即拉高 SCL 导致的 STOP 误判；
  - 解码器支持**重复起始**（字节采集时检测 START 条件），修复 0xD0 错位；
  - 位级标注：地址位 A6..A0（紫）、R/W 读写位（橙）、数据位 b7..b0（蓝）、
    ACK 位、字节标签 `ADDR 0x50 (0xA0) W`（7 位地址+总线字节+方向）。
- **鲁棒性多组验证**（全部 PASS）：
  - 不同地址：0x55/0x00(广播)/0x7F(最大) 数据全对；
  - 边界数据：0x00/0xFF/0x7F/0x80 逐字节正确；
  - 复杂帧 0x5A：重复起始+读+NACK 完整解析；
  - 单字节帧 0x00/0xFF 正常；
  - 10 位地址前缀帧（0x78）：按 7 位地址解析，不崩溃；
  - 快速连续切换帧源：无残留/错乱。

### 12.7 上位机体检与协议极限评估
- **体检结论**：
  - I2C 早期观察到的 STOP 数量偏多为**采样窗口边界截断**所致（简单帧 11/11、
    复杂帧 7/7 完全匹配），非解码器缺陷；
  - 传输层健壮：`la_dma_stop` 无响应/下载超时均有显式报错；帧 CRC 校验、
    滑窗重同步正常；
  - 低风险项：`capture.py` 缓冲大小硬编码（32768/8192），与固件宏耦合，
    后续可改为协议查询。
- **极限评估（采样引擎 10MHz 配置上限 / 5MHz 充分实测）**：
  - UART：实测 **921600** @5MHz；理论 ~1.25MHz @10MHz（≥8 样本/bit）；
  - SPI：实测 **164kHz** @5MHz；理论 ~330kHz @10MHz（≥30 样本/bit 边沿检测）；
  - I2C：实测 **100kHz** @1MHz；理论 ~1MHz @10MHz（≥10 样本/bit）；
  - 当前信号发生器即实测瓶颈：UART 921600 / SPI 164kHz / I2C 100kHz，
    常见工业场景全覆盖；高速场景需信号源提速 + 10MHz 专项验证。
- **自动化测试脚本**：`HOST/LogicAnalyzer/tests/test_robustness.py` 提供
  UART/SPI/I2C 各 20 组实机帧矩阵（可分段运行 `uart|spi|i2c`），
  注意协议切换需对应换线（UART:PC6 / SPI:PB13·15·12 / I2C:PE2·3）。

### 12.8 OTA 全面升级：四区分区 + A/B 回滚体系 + APP 运行时下载
- **分区重构（1MB 扇区对齐）**：
  - BOOT 64K（0x08000000）/ RUN 320K（0x08010000，APP 链接地址不变）/
    BACKUP 384K（0x08060000）/ DOWNLOAD 128K（0x080C0000）/
    PARAM 128K（0x080E0000）。
- **BOOT 升级**：
  - 启动状态机：NORMAL / PENDING（新固件待确认）/ RECOVERY（回滚超限）；
  - 升级前自动备份当前 RUN → BACKUP；新固件启动计数超限自动回滚；
    RUN 损坏自动从 BACKUP 恢复；连续回滚 5 次进入 RECOVERY 等待强制重刷；
  - 参数区双份冗余（magic+CRC32，写前整扇擦除写两份）。
- **APP 升级**：
  - 运行时 OTA 服务：HOSTLINK 新增 `CMD_OTA_BEGIN/DATA/END/STATUS`
    （0x08~0x0B），块写入 DOWNLOAD 区（offset 连续性校验 + 越界防护）；
  - 版本降级在 APP 端提前拦截，BOOT 侧二次校验；
  - 启动确认：新固件首次正常运行后向参数区写 NORMAL（未确认则 BOOT 回滚）；
  - 下载完成置备份域升级标志复位，BOOT 负责备份/切换/验证/解密。
- **验证**：BOOT/APP Keil 0 Error 0 Warning；GCC 双路径构建通过；
  待实机验证：正常启动 → 升级 → 回滚全流程。

### 12.9 OTA 实机验证与关键 Bug 修复
- **实机验证（YMODEM 路径，DAP 故障下串口升级）**：
  - 传输 62 帧 100% 接收、CRC 匹配、安全验证通过、AES 解密写入 RUN；
  - 新固件复位后正常启动（Module registry 9 模块 + Event Bus Ready）；
  - 期间发现上位机 `cryptor.py` UID 派生字节序与固件不一致（fromhex vs
    uint32 小端），已修复并与 BOOT `AES_KEY_PREFIX` 对齐验证。
- **关键 Bug：BKP 备份寄存器 HAL 参数错误**：
  - `HAL_RTCEx_BKUPWrite/Read` 第二参数应为**寄存器索引 0..19**，而非地址；
  - APP `BSP_RTC_WriteBackupReg` 与 BOOT `BKP_READ/WRITE` 均误传
    `RTC_BKP_DR1 + offset` 地址值，导致升级标志从未真正写入、
    APP→BOOT 升级触发失效（复位后 BOOT 走 NORMAL 而非升级模式）；
  - 已修正为索引访问（BKP_DR1 = 索引 0）。
- **升级触发双保险**：APP `Ota_End` 除 BKP 标志外，同时向参数区写
  PENDING + boot_count=MAX（BOOT PENDING 超限路径经回滚进入升级模式接收
  固件），不依赖备份域电池。
- **部署状态**：修复代码 Keil/GCC 编译通过；因 DAP 故障无法烧录 BOOT，
  需恢复调试器后烧录修复版 BOOT + APP 再完成回滚/确认全流程验证。

### 12.10 OTA 参数区持久化与启动确认实机验证（三连击根治）
- **实机现象**：升级后 `boot_param_save` 回读 state=2(PENDING) 成功，但复位后
  BOOT 读到 state=1(NORMAL)——PENDING 无法跨复位持久。
- **根因一（CRC 自引用）**：`boot_param_crc` 把 crc32 字段自身纳入 CRC 计算：
  save 基于旧 crc 算值、写入新值后 load 用新值重算必然不等 → 参数永远校验失败。
  修复：CRC 只计算 `offsetof(结构体, crc32)` 之前的数据字段（BOOT/APP 同步）。
- **根因二（SOP/OPTERR 残留）**：RDP 解除后 `FLASH_SR_SOP` 操作错误位残留，
  HAL_FLASHEx_Erase 检查到即返回 HAL_ERROR（st=1 err=0）。修复：APP 擦除前
  清全部错误位（含 OPTERR），BOOT 擦除掩码补 SOP。
- **验证通过**：
  - 升级写 PENDING → 复位后 `BOOT state=2 tries=1`（持久化成功）；
  - PENDING 分支 count++ → `Pending boot #2` → 跳 APP；
  - APP 启动确认 `erase=1 write0=1 write1=1` → `startup confirmed OK`；
  - 完整链路：升级→备份→切换→启动确认全部实机验证。

### 12.11 回滚恢复失败根治：`flash_copy_raw` 缺失 PSIZE（PGPERR）
- **现象**：`ota_rbtest` 置 PENDING+count=3 复位后，BOOT 稳定复现
  `New APP failed 3 tries, rolling back...` → `Rollback failed, entering upgrade mode.`，
  而 BACKUP 区校验有效、RUN 区擦除也报告成功。
- **调试过程（逐层收窄，三层日志）**：
  1. 先给 `boot_restore_backup` 加 `[RB]` 日志 → `erase=1 copy=0`，锁定失败在
     `flash_copy_raw`（BACKUP→RUN 复制），擦除与 BACKUP 校验均正常；
  2. 再给 `flash_copy_raw` 错误分支加**失败点寄存器快照**（地址 + 待写数据 + SR + CR）：
     `[COPY] FAIL addr=0x08010000 word=0x20014ED8 SR=0x00000040 CR=0x00000031`；
  3. 对照 ST 官方 SVD 解码：`SR=0x40` = **PGPERR（Programming Parallelism Error，
     编程并行度错误）**，且失败发生在**第一个字**。
- **根因**：`flash_copy_raw` 用寄存器级编程写 32 位字，却**未设置 PSIZE 字段**。
  STM32F407 复位后 `FLASH_CR.PSIZE=00` 表示 **x8 字节编程**，写入 32 位字即触发 PGPERR；
  而 HAL 的 `HAL_FLASH_Program` 会先 `FLASH->CR |= FLASH_PSIZE_WORD(0x200)`（x32）。
  因此 OTA 下载（HAL 路径）一切正常，回滚复制（裸寄存器路径）必然失败——同源代码两种命运。
- **修复**：`flash_copy_raw` 解锁后、写循环前显式
  `FLASH->CR &= ~FLASH_CR_PSIZE; FLASH->CR |= FLASH_PSIZE_WORD;`，
  并顺带清掉擦除遗留的 SNB 扇区号，保持 CR 干净。
- **验证**：`[RB] erase=1 copy=1` → `Rollback OK, jumping to APP...` →
  参数区写 NORMAL（回滚计数 +1）→ APP 正常启动。
- **复盘要点**：寄存器级 Flash 编程必须显式设置 PSIZE；排查疑难问题优先在
  失败点抓寄存器快照，比猜代码快得多。

### 12.12 运行时 OTA 全链路打通：BOOT 预下载镜像直通（业务不中断升级）
- **缺口**：HOSTLINK 运行时 OTA 由 APP 把固件包写入 DOWNLOAD 区后复位，但 BOOT
  `boot_enter_upgrade_mode` **无条件擦除 DOWNLOAD 区并进入 YMODEM 接收**，
  预下载数据被毁、切换无从谈起——"运行时下载"形同虚设。
- **重构**：把"校验 → 备份 RUN → 擦除 → 解密 → 写魔数/版本 → 置 PENDING → 重启"
  抽成 `boot_apply_download()`（YMODEM 收包后与预下载路径共用同一实现）；
  升级模式入口先探测 DOWNLOAD 区包头（`magic==0x4F5441FE` 且
  `0 < firmware_size <= DOWNLOAD_SIZE - 头 - 签名`），命中则直通切换，
  校验失败才回退 YMODEM。重构采用脚本化精确行替换并留 `.bak_refactor` 备份。
- **验证（连续两次完整链路 v13→v14→v15）**：
  ```
  APP : begin v15 → download complete (63312 B in 2.60 s) → rebooting
  BOOT: Pre-downloaded package found, applying directly...
        Current APP version: 14 → Backup → Security passed → PENDING=1 → reboot
  APP : new firmware confirmed → startup confirmed OK
  ```
  升级全程 APP 业务不中断，仅最后复位切换。

### 12.13 DAP 失效排查与 UART bootloader 恢复通道
- **现象**：Keil 报 `Flash Download failed - Target DLL has been cancelled`，
  APP/BOOT 工程一致，几分钟前还能正常烧录。
- **调试方法（绕过 Keil，直连 CMSIS-DAP HID 探测）**：
  - 枚举确认 DAP（VID_C251/PID_F001，MuseLab 方案）HID 接口在线；
  - 用 Windows HID API 直接发 CMSIS-DAP 原始命令：`DAP_Info` / `DAP_SWJ_Clock` /
    `DAP_Connect(SWD)` / `DAP_SWJ_Sequence` 全部正常应答 → 适配器固件存活；
  - `DAP_Transfer` 读 DPIDR 返回 **0 次传输完成**（SWD/JTAG 双模式均失败）→
    目标芯片不应答；nRESET 也拉不低；
  - 双串口交叉验证：COM9 有 BOOT ymodem 日志、COM13 有 `C` 轮询 → **MCU 活着**，
    只是 DAP↔芯片 SWD 物理链路断开（代码层已排除 PA13/14 复用与 RDP）。
- **恢复通道（不依赖 SWD）**：BOOT0=1 + 复位进系统 bootloader，
  `STM32CubeProgrammer -c port=COM13 br=115200`（UART1）全片擦除 → 烧 BOOT →
  烧 APP（RUN/BACKUP）→ 校验，全程可复现、可脚本化。后续用户重新接好排线后
  DAP 自行恢复（Erase/Programming/Verify OK）。
- **复盘要点**：DAP 报错 ≠ 适配器坏；HID 直连探测能在 5 分钟内区分
  "适配器故障 / 芯片失联 / 物理接线"。

### 12.14 HOST 工具下载慢 50 倍提速：pyserial `read(size)` 阻塞陷阱
- **现象**：HOSTLINK 下载 63 KB 耗时 136.8 s（0.46 KB/s），每块响应固定 ~505 ms。
- **排查过程（一步步排除 MCU）**：
  - `taskstats` 显示系统空闲、IDLE 在跑；tick=1ms 正常；
  - 单字符回显 1ms、10 字节突发用 `read(1)` 逐字节读回显 3ms → **MCU 全速**；
  - 同一突发改用 `read(256/512)` 批量读 → 稳定 500ms —— 差异只在**读取方式**。
- **根因**：pyserial `read(size)` 的 Windows 语义是"等到 `size` 字节或超时"，
  数据不足 `size` 时即使有数据也会**等满超时**才返回；`ota_worker.py` 里
  `ser.read(512)` 每块响应白等 ~0.5s，264 块 × 0.5s ≈ 136s。
- **修复**：改为按实际可读字节数读取：
  `n = ser.in_waiting; d = ser.read(n) if n else b''`。
- **验证**：同链路 2.60 s 完成 63312 B（24 KB/s，受 MCU Flash 写速限制），提速 50 倍。
- **复盘要点**：串口性能测量必须用 `in_waiting` 或 `read(1)` 逐字节；
  大 size 的阻塞 `read` 会把"响应延迟"误判成"设备慢"。

### 12.15 安全清理：AES 派生密钥前缀泄露
- **问题**：`derive_aes_key()` 向调试串口打印 `AES_KEY_PREFIX: 4D4A7E95...`
  （密钥前 8 字节），泄露密钥材料且污染启动日志。
- **修复**：删除该调试打印；BOOT/APP 两侧密钥派生逻辑不变。
- **验证**：最终 BOOT 0 Error / 0 Warning，启动日志无密钥痕迹。

---

## 13. 日志跟踪规范（2026-08-07 起）

> 目的：让每一次问题、修复、实机验证都有据可查，随时可复盘、可追溯。

- **记录时机**：发现问题 / 定位根因 / 完成修复 / 实机验证 / 里程碑达成的
  **每一个节点**都追加一条记录，不攒、不补写。
- **条目模板**（保持与上文一致的"现象 / 根因 / 调试 / 修复 / 验证"结构）：
  | 字段 | 说明 |
  | --- | --- |
  | 现象 | 可复现的现象与关键日志摘录 |
  | 根因 | 一层到底的原因，含证据（寄存器值 / 协议帧 / 时序） |
  | 调试方法 | 可复用的排查手段（快照、探测、交叉验证等） |
  | 修复 | 改动文件与要点 |
  | 验证 | 实机/构建证据，0 Error 0 Warning 等 |
- **工作流约定**：改代码 → Keil 编译 → DAP 烧录 → 实机验证 → **同步写日志**；
  仅当重大验证全部通过后才 git 提交（用户既定纪律）。
- **编号规则**：按 `章节.小节` 递增（如 12.16、13.1），不重排旧编号。

### 12.16 P2 · 固件元数据扩展 + 防重放（chip_id / build_no）
- **设计**：`ota_header_t` 的 `reserved[8]` 语义化为 `chip_id[4] + build_no[4]`
  （32 字节布局不变、签名覆盖范围不变，BOOT 与 HOST 打包同步）。
- **BOOT**：`security_verify_and_decrypt` 新增两道防线——
  芯片型号匹配（`header.chip_id != (DBGMCU->IDCODE & 0xFFF)` → 拒绝，防跨芯片烧录）；
  防重放（`header.build_no <= 参数区 last_build_no` → 拒绝，防同版本重放）。
  参数区 `boot_param_t` 增加 `last_build_no`（CRC 用 offsetof 自动纳入，BOOT/APP 同步），
  切换成功写 PENDING 时一并持久化。
- **HOST**：`cryptor.py` / `ymodem_sender.py` 包头改 `struct.pack('<III12sII', ...)`；
  新增 `version_lib.py` 版本库（build_no 单调分配 + 固件登记）。
- **实机验证**：
  - 正常链路 v17 build=2：`Current APP version: 15, last build: 0` →
    `Security verification passed` → 切换 → `OTA Agent ready (last build 2)`；
  - 防重放：重放同 build 包 → `[SEC] Replay denied! build=2 last=2` → 拒绝并安全回退；
  - 芯片校验：`chip=0x999` 包 → `[SEC] Chip mismatch! pkg=0x0999 dev=0x0413` → 拒绝；
  - YMODEM 路径兼容新包头（v19 升级成功，62 帧全确认）。

### 12.17 UPGRADE 状态归一化修复（防"卡死在升级模式"）
- **问题**：OTA 校验失败回退 YMODEM 后，参数区 `boot_state` 残留 UPGRADE(4)，
  下次复位会再次进入升级模式；叠加 YMODEM 模式阻塞 SWD，板子容易被"锁"住。
- **修复**：BOOT 的 BKP 升级标志分支进入升级模式前先写
  `boot_state=NORMAL, boot_count=0` 并保存——无论本次升级成败，复位后都回 APP。
- **验证**：`[PARAM] save OK (state=1 count=0)` → 校验拒绝 → 复位自动回 APP 正常运行。

### 12.18 已知现象：BOOT YMODEM 模式阻塞 SWD 连接（三次复现）
- **现象**：板子处于 BOOT 升级模式（YMODEM 等待握手）时，Keil DAP 报
  `Target DLL has been cancelled`；HID 直连探测 `DAP_Transfer` 读 DPIDR 返回
  0 次传输完成（目标不应答）。复位回 APP 后 DAP 立即恢复（第三次复现确认非巧合）。
- **影响**：不影响 OTA 升级功能；烧录前需避开 YMODEM 模式（先复位，或用 BOOT0+UART 恢复）。
- **待查**：疑与升级模式下 Flash/SWD 交互有关（工程内已有"擦 flash 阻塞 SWD"注释），
  留作专项排查项。

### 12.19 上位机增强：版本库 / 阶段可视化 / 批量升级 / 彩色日志
- `version_lib.py`：固件版本库（chip_id 配置、build_no 单调分配、条目登记，JSON 持久化）；
- `ota_worker.py`：新增 `stage_signal`（IDLE→DOWNLOADING→VERIFYING→COMMITTED→RUNNING），
  打包自动从版本库取 build_no；
- `main_window.py`：升级阶段徽章、版本库下拉（选择自动填充固件/版本）、
  批量端口并发升级（每端口独立 worker）、日志按级别着色；
- **验证**：5 个 HOST 模块 ast 语法检查全绿；版本库分配/登记实测通过。

### 12.20 OTA 上位机界面精修（v2.1.0）
- **布局**：窗口 900×760（最小 820×660），日志区弹性占位；
  自上而下：串口设置 → 固件/安全配置（含版本库）→ 操作按钮 →
  批量端口 → 模式提示 → **升级阶段流程条** → 进度条 → 升级日志。
- **阶段流程条**：IDLE→DOWNLOADING→VERIFYING→COMMITTED→RUNNING 五段，
  已完成段绿色、当前段蓝色、未到段浅灰，直观展示升级推进。
- **视觉**：主按钮"开始升级"绿色加粗强化、批量按钮紫色、输入框 hover 描边、
 复选框圆角、日志自动滚动到底部。
- **验证**：布局程序化检查（GroupBox/阶段条/控件全就位）、PyQt 实际渲染截图
 （待命态 + 升级态）正常生成；ast 语法全绿。

### 12.21 启动早期擦参数扇区 BSY 卡死（重大根因，多轮定位）
- **现象**：HOSTLINK END 升级时 BOOT 稳定卡死/复位——`[PARAM] pre-erase` 后
  Flash BSY 永久置位（IWDG 禁用时 >70s 不完成）。而 ota 命令触发（参数区 0xFF）、
  CubeProgrammer 写入（系统 bootloader）、升级模式内擦除全部正常。
- **定位过程**：排除内容/缓存/中断/槽位组合（多轮对照实验）后，锁定
  **BKP 分支的 `boot_param_save`**（12.17 为归一化状态新增）：移除后 END 升级立即恢复；
  CubeProgrammer 写 state=4 走 param 分支（无 BKP_WRITE）正常。机制疑与
  启动早期（BKP_WRITE 后立即）Flash 操作时序相关，留作专项排查。
- **修复**：BKP 分支不再提前保存参数；状态归一化移到升级模式内
  （探测失败跳 APP 前执行 `boot_param_save`，此时延迟后擦除正常）。
- **验证**：END 升级 v38/v43/v58 全绿；防重放拒绝 → 归一化（state=1）→ 跳 APP → 复位不卡。

### 12.22 跨复位断点续传（下载一半 → 断电/复位 → 精确续传）
- **设计**：DOWNLOAD 区尾部 16KB 会话槽区（512 槽 × 32B），每块精确记录 received
  （恢复点 = 实际已写位置，避免重写已写区域）；BEGIN 检测同版本会话则续传不擦除；
  HOST 升级工具 BEGIN 后查询 STATUS 从断点继续。
- **三个隐蔽 Bug（逐一定位）**：
  1. 槽粒度过粗（received/3840）→ 同一槽反复写被 AND 破坏（实测槽区 received 值混乱）；
     改为每块写精确进度；
  2. `ota_flash_write` 失败路径泄漏 `__disable_irq()` → SysMon 无法喂狗 → IWDG 复位；
     所有失败路径补 `__enable_irq()`；
  3. 恢复路径不擦除，Flash 控制器残留状态导致写 0xFF 区域编程 BSY 卡死（HAL_TIMEOUT）；
     恢复时先清错误标志 + Unlock/Lock 重置控制器。
- **验证**：下载 32160/64192 → reset → BEGIN 恢复 rx=32160 → 续传至完成 → END →
  BOOT 校验应用 → 升级成功 + 启动确认闭环。

### 12.23 可靠性增强与安全清理
- BOOT IWDG 64 分频 ≈8.2s（原 4s），消除 320KB 擦除+复制超时风险；
- 签名私钥移出 `config.json` 明文，GUI 改由环境变量 `OTA_PRIVKEY` 注入；
- APP `ota_flash_write` 全程关中断（防多任务下 HAL 编程序列被中断破坏）。

### 12.24 BOOT 升级状态帧实时回传（上位机真实阶段可视化）
- BOOT 升级模式经 UART1 广播 HOSTLINK 状态帧（CMD 0x0C：阶段+错误码+版本），
  发送前临时切换 921600 波特率（与 HOSTLINK 数据口一致）后恢复；
- 阶段：PREP/VERIFY/BACKUP/ERASE/WRITE/COMMIT/DONE/FAIL；YMODEM 路径不广播（防干扰）；
- 上位机解析 0x0C 帧 → 阶段流程条显示真实推进（不再纯流程模拟）；
- 实测：END 后捕获 ERASE(4.7s)→WRITE(7.7s)→COMMIT→DONE，升级成功。

### 12.25 YMODEM 阻塞 SWD 根因确认（12.21 修复连带解决）
- 重测：YMODEM 模式下 DAP 烧录正常（Erase/Programming/Verify OK）——
  此前"YMODEM 阻塞 SWD"实为启动早期 Flash BSY 卡死阻塞 AHB 的连带现象，
  12.21 移除 BKP 分支提前 boot_param_save 后彻底消失。

### 12.26 Flash 读保护（RDP）脚本化支持
- 新增 `HOST/OTA_Tool/enable_rdp.py`：CubeProgrammer 设置 RDP Level 1（0xBB），
  附前置条件（BOOT0=1）与影响说明（防提取、解除触发全片擦除、OTA 不受影响）；
- 生产发布前执行；开发调试阶段保持 RDP 关闭。

### 12.27 签名密钥轮换 + 私钥安全治理
- 修复：`BOOT/Script/ymodem_test.py` 硬编码私钥泄露 → 改为环境变量 `OTA_PRIVKEY` 注入；
- 生成新生产密钥对（私钥仅终端输出/安全保管，不入仓库）；
- BOOT 双公钥验证（`ECDSA_PUB_KEY` 新 + `ECDSA_PUB_KEY_LEGACY` 旧）：
  旧密钥包与新一代包均可验证，过渡期后移除 LEGACY 完成轮换；
- 新增 `HOST/OTA_Tool/gen_keys.py`（安全生成 + 轮换流程说明）；
- 清理 `version_lib.json` 乱码/过期条目；
- 实测：旧密钥包 v61 与新一代密钥包 v62 升级均成功。

### 12.28 断电注入系统测试（升级全生命周期断电恢复实证）
- 在 BOOT 应用流程加入编译期断电测试钩子（`POWERLOSS_TEST_STAGE`，只触发一次，
  用参数区 last_error 标记；生产为 0），逐阶段模拟断电并验证恢复：
  - 阶段1 备份后断电：RUN 完整 → 重启重新应用 → 升级成功；
  - 阶段2 擦除后断电：RUN 空 → 重启重新应用（跳过备份）→ 升级成功；
  - 阶段3 写入后断电：RUN 损坏无魔数 → 重启重新应用成功；另验证 BACKUP 自动回滚
    （`[RB] BACKUP valid → erase=1 copy=1 → Backup restored`）；
  - 阶段4 提交后断电：PENDING 已持久化 → 重启走确认分支 → 启动确认闭环；
  - 下载中断电：跨复位断点续传（12.22 已验证）。
- 结论：升级任何阶段断电均可自动恢复，最终要么完成升级、要么回滚到上一版，
  任何情况下存在可运行固件（A/B + 双确认体系闭环）。

### 12.29 启动早期擦参数扇区 BSY 卡死机制推测（已规避，证据充分）
- 现有证据：BOOT 启动早期打印 `ACR=0x00000705`（含 ICRST=1，缓存复位挂起）；
  早期（BKP 分支）擦参数大扇区 BSY 卡死；延迟后（升级模式内）擦除正常；
  CubeProgrammer（系统 bootloader）与 APP 长期运行后擦除均正常。
- 机制推测：启动早期 FLASH_ACR 缓存复位状态未稳定（ICRST 挂起），
  大扇区擦除与缓存状态交互异常导致 BSY 卡死；延迟后缓存就绪则正常。
- 已通过架构规避（不再启动早期擦参数扇区）；底层寄存器交互留作硬件级专项。

### 12.30 ?????? + ???????
- ????? Keil `-b`???????? 1s????????? `-r`?
- ???? OTA_Tool ??????/???/???????/????

### 12.31 上位机实机操作验证（GUI 全流程通过）
- 实机操作 OTA_Tool 上位机完成一次完整 HOSTLINK 升级（v69 build 55）：
  版本库下拉选择固件 → 私钥环境变量注入 → 下载 → END 触发 →
  BOOT 状态帧实时显示（安全校验/备份/擦除/写入/提交/完成）→ 升级成功 + 启动确认闭环；
- 修复三处：
  1. `ota_worker.py` 缺 `version_lib` 导入（`load_lib is not defined`）——补导入；
  2. END 无响应误报"失败"——`Ota_End` 成功路径直接复位不回复 ACK，
     改为"END 已触发，设备复位中"并继续监听 BOOT 状态帧，最终报成功；
  3. 私钥注入状态可视化：绿色"✓ 环境变量已注入"/橙色"未设置(可手动粘贴)"。

### 12.32 OTA 架构全貌文档（收尾）
- 新增 [OTA_ARCHITECTURE.md](OTA_ARCHITECTURE.md)：系统总览 / Flash 分区 /
  模块架构图 / 启动状态机 / 核心功能原理 / 升级方法与流程时序 /
  安全模型 / 测试验证记录 / 已知边界 / 复盘索引；
- 本阶段 OTA 体系定稿：A/B+确认、回滚、运行时 OTA、断点续传、断电全生命周期、
  安全五重防护（签名/加密/芯片/防重放/防回滚）、密钥轮换、状态帧可视化全部闭环。

### 12.33 LA_DUMP 大块传输卡死（根因 + 三合一根治）
- **现象**：HOSTLINK `CMD_LA_DUMP` count≤512 正常（87KB/s），count≥4096 只收 ~42B 后卡死。
- **根因（两层）**：
  1. **生产者快于消费者**：每帧固定延时 2ms，但 921600 波特率下 253B 帧排空需 ~2.75ms，
     生产者比消费者快约 37%；8 深队列持续导出必然灌满，超出帧静默丢弃，上位机永远等不齐样本数。
  2. **RX IDLE 误杀 TX**：`HAL_UART_DMAStop`（空闲断帧用）会连同进行中的 TX DMA 一起中止，
     且中止后不触发完成回调 → TX 任务永久挂起。
- **修复（v76）**：
  - `data_link.c`：LA_DUMP 改**背压式发送**（`DataLink_SendFrameWait` 队列满阻塞，与线速自匹配），
    去掉固定延时，绝不静默丢帧；新增 TX 丢帧/错误计数，sysmon 可查。
  - `bsp_uart.c`：**TX/RX 隔离**——IDLE 断帧遇 TX 进行中则推迟到 TX 完成回调处理；
    新增 `HAL_UART_ErrorCallback`（仅 TX 错误才唤醒，防 RX 错误误清忙标志导致并发 DMA）；
    新增 `BSP_UART_AbortTransmit` 自愈接口。
  - `TXTask`：DMA 完成等待加 2000ms 超时 + 强制中止自愈，任何丢通知/中止/错误不再永久挂起。
- **验证**：LA_DUMP 512/4096/8192/32768 全量通过（547 帧，86.4KB/s），
  sysmon `TX frames lost: 0` / `TX errors: 0`。

### 12.34 OTA 实机升级验证（LEGACY 密钥 + 防重放 build_no + CLI 工具）
- **背景**：BOOT0 拨码太麻烦，改走**运行时 HOSTLINK OTA**（无需跳线/DAP）。
- **密钥**：新生产私钥不落盘；开发验证用 git 历史中的 LEGACY 私钥
  （与 BOOT 内嵌 `ECDSA_PUB_KEY_LEGACY` 完全匹配，双公钥兼容）。
- **防重放教训**：首次 build 56/57 被 BOOT 拒绝——`[SEC] Replay denied! build=57 last=58`，
  板上参数区 `last_build_no=58`（此前 OTA 测试已占用更高 build），version_lib 记录已失真；
  必须**严格递增**（59/60/61），并抓 COM9 日志确认真实原因。
- **新增工具**：`Script/ota_hostlink_cli.py`（无 GUI 依赖的 HOSTLINK OTA CLI）、
  `Script/ota_capture_boot_log.py`（COM9 日志抓取 + 复现触发）。
- **验证**：v76(build59)/v77(build60)/v78(build61) 三次 OTA 全链路成功，
  BOOT 状态帧 VERIFY→BACKUP→ERASE→WRITE→COMMIT→DONE 全部 err=0。

### 12.35 CPU 极限负载测试与信号发生器 DMA 化（极致优化）
- **压力场景**：LA 6MHz DMA 采样 + USART6 921600 + SPI2 164kHz + HOSTLINK 全量导出同时运行。
- **优化前**：SG_SPI 阻塞式 `HAL_SPI_Transmit`（390µs/帧）占 39% CPU，SG_UART 阻塞占 18%，
  峰值 ~47%。
- **优化（v77/v78）**：信号发生器 SPI2（DMA1_Stream4_CH0）与 USART6（DMA2_Stream6_CH5）
  全部改 **DMA 传输 + 任务睡眠等待完成**；BSP 提供 `BSP_UART_OnTxComplete` 弱钩子
  扩展非 BSP 通道（无参解耦，避免 HAL 头依赖）。
- **最终数据**：
  | 场景 | SG 任务 CPU | 峰值 CPU | 传输速率 | 丢帧/错误 |
  | --- | --- | --- | --- | --- |
  | LA6M+SPI2+HOSTLINK | 0% | ~9%（DL_CMD 帧解析） | 85.3KB/s | 0/0 |
  | LA6M+USART6+HOSTLINK | 0% | ~9% | 85.4KB/s | 0/0 |
  | 空闲基线 | - | 1% | - | - |
- **内存**：堆富余 10.2KB（满载）/ 10.8KB（空闲）；栈高水位全部充裕
  （DL_CMD 137/2048、DL_TX 151/1024、SG 91/512、eventBus 262/1536）。
- **结论**：事件驱动 + DMA 卸载架构合理，真实业务满载 CPU 仅 ~9%，余量极大；
  剩余峰值开销为 HOSTLINK 协议帧解析（32 位 MCU 流式处理必要成本），无可再压。

### 12.36 事件总线极限负载测试 + 喂狗中断化加固（重大根因）
- **测试工具**：新增 `eb_stress <count> [payload] [burst|steady]` shell 命令
  （`MSG_EB_STRESS` 消息 + 订阅端计数 + DWT 168MHz 周期计时）；
  burst=挂起 eventBusTask 测纯发布，steady=实时消化测系统吞吐。
- **极限数据（v80/v81 实测）**：
  | 指标 | 数值 | 说明 |
  | --- | --- | --- |
  | 突发发布速率 | ~76 万次/s（221 cycles/次） | 挂起消费者，纯 AllocMsg+Publish |
  | 稳态系统吞吐 | 40.6k msg/s（payload=0）/ 35.0k（payload=128） | 发布+消费联合，持续 50 万条零丢失 |
  | 并发在途上限 | 32 条 | 静态池大小=架构约束，队列 64 为冗余缓冲 |
  | payload 影响 | 0/64/128 字节 cpmsg=222/223/224 | 验证"传指针"设计，复制不在总线路径 |
  | 过载行为 | 按设计丢弃（回收+计数），发布者不阻塞、不崩溃 | Lost 计数可查 |
- **重大缺陷发现**：硬件喂狗依赖**低优先级 Tmr Svc 软件定时器**——`eb_stress 200000 steady`
  （~5s 连续事件风暴）饿死 Tmr Svc → **IWDG 复位**（sysmon RESET REASON=Independent watchdog reset）。
- **修复（工业级原则）**：喂狗改为 **SysTick 中断钩子**（`configUSE_TICK_HOOK=1` +
  `vApplicationTickHook` 每 1000 tick 喂一次），任何任务调度风暴都不影响喂狗。
- **验证**：修复后 `eb_stress 500000 steady`（12.3s 连续风暴）**500000/500000 零丢失、零复位**；
  v80/v81 OTA 全链路成功。
- **架构结论**：事件总线"静态池+指针队列+最高优先级分发任务"设计合理，吞吐受
  eventBusTask 消化速率约束（40k msg/s 量级），真实业务（<1k msg/s）余量 ≥40 倍；
  池大小 32 为内存与并发上限的平衡点，按需可调。

### 12.37 顶级纠错系统（崩溃诊断 + 原因复现 + 安全恢复）
- **设计**：新增 `SystemServices/err_mgr.{c,h}` 错误管理模块，覆盖：
  - Cortex-M4 全部 fault（NMI/HardFault/MemManage/BusFault/UsageFault）——
    5 个 **汇编入口直接占用中断向量**（naked/__asm 在进入 C 前捕获真实 EXC_RETURN
    与栈指针，按 EXC_RETURN.bit2 选 PSP/MSP），不再死循环；
  - 未处理中断：startup 弱 handler 汇聚点改为**读取 VECTACTIVE 进统一诊断**；
  - RTOS 层：configASSERT（带 __LINE__）、栈溢出钩子、任务看门狗 stall 全部接入。
- **现场采集**：寄存器/栈帧/CFSR/HFSR/BFAR/MMFAR 解码、当前任务名（pxCurrentTCB 直读）、
  堆栈回溯（Flash 范围+Thumb 位启发式扫描，SRAM 边界保护防二次 fault）、
  EXC_RETURN 线程/处理模式判别。
- **持久化与复现**：摘要存 RTC 备份寄存器（reg 1-15，reg 0 保留给 OTA），
  含 CRC 校验；启动时 `ERR_ReportLastCrash()` 自动打印上次崩溃原因（[CRASH] 复现）。
- **恢复策略**：默认**软复位**（LED 快闪提示）；RTC 秒级窗口检测连续快速崩溃
  （10s 内 3 次）→ **防抖锁定**（喂狗+慢闪等待人工断电，防复位风暴）。
- **崩溃注入工具**：`crash <bus|undef|stack|assert|irq>` shell 命令
  （`CRASH_INJECT_ENABLE` 开关，生产发布置 0）。
- **实机验证（v82-v92）**：5 种来源全矩阵通过，崩溃序号 #1-#5 递增、
  完整诊断输出、复位后自动复现、防抖锁定生效（3 次快速崩溃后 12s 无复位）。
- **调试中发现并根治的 4 个关键坑**：
  1. 汇编入口必须**直接作为中断向量**（C 包装的 BL 调用破坏现场寄存器）；
  2. `HAL_UART_AbortTransmit` 在中断上下文可能阻塞 → 转储改裸寄存器
     （直接禁 DMA 流 + 清标志 + 轮询 TXE）；
  3. `xTaskGetCurrentTaskHandle()` 内部 `taskENTER_CRITICAL` 在 ISR 非法 →
     改为直接读 `pxCurrentTCB`（task.h 未导出，extern 声明）；
  4. `xTaskGetTickCountFromISR()` 在最高优先级中断（优先级 0）触发 FreeRTOS
     优先级断言 → 递归 assert 死循环 → 改为 SysTick 钩子维护 `ERR_TickMs` 快照。
- **sysmon**：新增 "Last Crash" 监控项（崩溃序号 + 原因），复位原因独立显示。

### 12.38 BOOT 轻量纠错系统（bootloader 专用精简版）
- **背景**：BOOT 全部 fault handler 原为**空死循环**（仅靠 IWDG 兜底，崩溃无现场信息）。
- **设计原则**：bootloader 比 APP 更精简——无 RTOS（无任务名/回溯/钩子），
  不做防抖锁定（BOOT 必须能自动恢复，持续崩溃由升级流程兜底）。
- **实现**（`BootServices/boot_err.{c,h}`）：
  - 5 个 fault（NMI/HardFault/MemManage/BusFault/UsageFault）汇编入口直接占向量，
    捕获真实 EXC_RETURN 与栈指针 → 统一 C 入口；
  - 裸 UART2 诊断输出（fault 类型/PC/LR/寄存器/CFSR/HFSR，不依赖 printf/HAL）；
  - **软复位恢复**（绝不死循环）；
  - 崩溃摘要持久化 BKP **reg 16-19**（与 APP err_mgr 的 reg 1-15 隔离，
    reg 0 保留给 OTA 标志），CRC 校验；
  - 启动时 `Boot_ErrReportLast()` 自动复现上次 BOOT 崩溃（main.c 集成）。
- **验证**：BOOT 编译 0 Error（1 个 ARMCC #111-D 固有误报——还原对比确认与改动无关）；
  DAP 烧录 Erase/Program/Verify OK；复位后启动链正常（BOOT→APP 无回归），
  无崩溃记录时复现函数正确静默。崩溃路径机制与 APP 已实机验证方案同构。

### 12.39 项目清理 + BOOT/APP 启动打印重构
- **项目清理**：删除 453 个未跟踪构建产物/临时文件（build_*.log、flash_*.log、
  .bak/.tmp/.pyc、_ota_*.bin、temp_secure*.bin、旧 APP_flash*.bin）、14 个
  __pycache__ 目录、VLink_Debugger 的 PyInstaller build/dist；git 零变化
  （全部已忽略产物），编译中间产物（.o/.crf/.d）保留以支持增量编译。
- **启动打印重构**：
  - BOOT/APP 统一 **ASCII figlet 风格 "D00" logo** + 平台信息框
    （D00 Embedded Platform | STM32F407 Industrial Bootloader/Application）；
  - 统一 `[BOOT]`/`[APP]` 前缀与对齐（`标签 : 值`）；
  - BOOT：State 人类可读（NORMAL/UPGRADE/PENDING/RECOVERY）、跳转行带
    APP 版本（ver=94）、"BOOT Started." 由 banner 取代；
  - APP：banner 提前到模块注册之前，显示 Firmware 版本；
    模块注册对齐（`[%u] %-10s prio=%u`），LA/OTA/SysMon/LED 状态统一格式；
    收尾行 "Boot complete. Async Event Bus ready."。
- **发现并修复**：APP banner 超长导致 `LOG_Printf` 256B 内部缓冲截断乱码
  → 横幅拆分为多段打印；BOOT printf 流式输出不受限（无需拆分）。
- **验证**：复位实机启动日志全链路（BOOT banner→State→Jump→APP banner→
  模块注册→Boot complete）排版统一美观、无乱码；BOOT/APP 编译 0 Error。

### 12.40 Shell 升级为顶级交互（串口助手体验）
- **评估**：原 shell 具备行缓冲/回显/退格/命令表/help，但缺提示符、
  命令补全、历史记录、分组帮助——与串口助手交互不够优雅。
- **升级（v95/v96）**：
  - **提示符**：`D00> `（启动与每命令后显示）；
  - **Tab 命令补全**：唯一匹配自动补全，多匹配列出候选（名称+说明）；
  - **历史记录**：环形 8 条，方向键（ESC[A/B）上下浏览，去重，满则丢最旧，
    行重绘用 `\r+空格覆盖+\r`（不依赖 ANSI，兼容全部串口助手）；
  - **ESC 序列状态机**：完整解析方向键；
  - **help 重构**：`help` 分组列表（名称+英文说明对齐）、`help <cmd>`
    单命令详情、交互提示；未知命令带 hint；
  - **命令表扩展**：全部 30 个命令补充英文用途说明（串口帮助用英文，
    同时规避 ARMCC5 按 GBK 解析 UTF-8 源码时中文尾字节吞引号的编译坑）；
  - **info 增强**：附加 Free heap 显示。
- **验证**：实机测试 help/help <cmd>/Tab 补全/历史上下键/sysmon/info 全部正常，
  编译 0 Error / 0 Warning。

### 12.41 板载 2.8 寸 TFTLCD 驱动（官方移植）+ 系统面板 + 引脚重规划
- **重大发现**：探索者 V3 板载 2.8 寸 LCD 实际控制器为 **ST7789（ID=0x7789）**
  而非 ILI9341！此前自研驱动强制 ILI9341 序列导致持续乱屏。
- **方案**：移植正点原子官方探索者 F407 LCD 驱动
  （`BSP/LCD/lcd.c + lcd_ex.c + lcdfont.h`，多 IC 自动识别，
   ST7789/ILI9341/NT35310/ST7796 等），适配 D00 工程：
  - 兼容层替代官方 SYSTEM/sys（sys_gpio_set/af_set/pin_set、delay_ms/us）；
  - FSMC BANK4 由官方 lcd_init 寄存器级配置（读慢 15/60、写快 9/8 分离时序）；
  - 移除自研 bsp_lcd 与 fsmc.c 冗余 BANK4 配置。
- **调试历程关键教训**：
  1. FSMC 16 位模式 RS 地址线 A6 = HADDR bit7（偏移 0x80）；
  2. FSMC 读需慢时序（DATAST=60），否则读 ID 返回 0；
  3. MADCTL 需按真实控制器（ST7789 竖屏官方值）；
  4. 官方多控制器驱动是终极可靠方案（板上验证过）。
- **引脚重规划**（LCD 优先）：LCD 占用 PG12(CS)/PF12(RS)/PB15(背光)/FSMC 总线；
  LA 采样通道改为 **PG6/PG7/PG8/PG15**（原 PG12 让出）；
  信号发生器 SPI 改**软件 SPI**（PE5=SCK/PE6=MOSI/PF6=CS，释放 PB15）。
- **系统面板 lcd_app**：HOME(版本/CPU/堆/任务) / SYSTEM(任务栈水位) /
  BUS(事件总线+HOSTLINK 状态) 三页，按键短按切换，1s 局部刷新不闪烁。
- **验证**：LCD 显示完美（用户确认）、LA SRAM 自检 PASS、堆 10.8KB、
  事件总线/HOSTLINK 零丢失零错误、编译 0 Error 0 Warning。

### 12.42 LCD 驱动极致性能优化（字符 6 倍 / 清屏 45% / 写时序 12ns）
- **字符窗口连续写**：一次 SetWindow + RAM 连续写，替代官方逐点
  lcd_draw_point（每点 5 次 FSMC 写）——16 号字符 35µs/字（2.86 万字/s），
  较官方逐点提升约 6 倍；字体按列优先存储、行优先输出（匹配 MADCTL=0x08）。
- **清屏循环展开**：8 像素一组连续写，20.6ms/屏（3.72 MPix/s，提升 45%）。
- **FSMC 写时序提速**：ST7789 分支 18ns → 12ns（ADDSET/DATAST=2/2，
  官方 9341 同款验证值）。
- **性能基准**：新增 `lcd bench` shell 命令（DWT 精确计时，
  测清屏/填充/字符/字符串吞吐；结束自动清屏不污染应用显示）。
- **教训**：大区域单窗口连续写填充（lcd_fill 优化版）在 ST7789 上实测
  只覆盖部分区域（面板下 1/3 残留），回退官方逐行 SetCursor 方案
  （正确性优先）；字符连续写已验证可靠保留。
- **实测（v115）**：clear 20.6ms / fill 1.35ms(5000px 优化版) /
  char16 35µs / string 2.23ms；v117 面板完整、系统零回归。

### 12.43 LCD 整合为统一 BSP 接口（bsp_lcd）
- **架构**：新增 `BSP/bsp_lcd.{c,h}` —— 统一 `BSP_LCD_*` 接口
  （Init/GetId/GetWidth/GetHeight/SetOrient/ScanDir/Clear/Fill/
   Draw*/Show*/Backlight/Bench），与 bsp_gpio/bsp_uart 同风格；
  官方多 IC 驱动（BSP/LCD/，含 ST7789 识别与初始化）保留为内部底层，
  由 bsp_lcd.c 封装。
- **收益**：应用层（lcd_app/shell）只依赖 `bsp_lcd.h`，屏幕更换/底层升级
  不影响上层；颜色宏统一 `BSP_LCD_COLOR_*`，字符字体枚举 `BSP_LCD_FONT_*`。
- **验证**：lcd_app 三页面板经 BSP_LCD 接口工作正常；
  `lcd bench`（BSP_LCD_Bench）性能数据与封装前一致
  （clear 20.9ms / char16 35µs / string 2.23ms）；编译 0 Error 0 Warning。
