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
