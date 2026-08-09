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

### 12.44 LCD 窗口残留根因（多轮排查最终定位）+ 显示稳定化
- **根因**：官方 `lcd_set_cursor(x,y)` 只写 CASET/PASET 的**起点**（各 2 字节），
  窗口**终点残留**上一次的值——画过字符（小窗口 x1=7）后，后续 fill/clear
  只覆盖"起点到残留终点"的窄条，旧画面大面积残留（此前所有"残留/参杂/
  清不干净"现象的真正根源，非撕裂、非方向）。
- **修复**：`lcd_set_cursor` 写**完整单点窗口**（x0=x1, y0=y1）；
  `lcd_fill` 改 `lcd_set_window` 完整窗口 + 连续写（可靠且性能最优）。
- **配套稳定化**：
  - 测试命令（test/bench/dir/clear）进入测试模式，暂停 1s 面板刷新，
    并置屏幕脏标记（任何外部绘制不被打扰）；测试画面保持，按键恢复 HOME；
  - 全屏清屏/切页后等待 40ms（防面板刷新撕裂）；
  - 字符间距 +1px（避免相邻字符视觉贴合）；
  - SYSTEM 页任务列表固定排序（优先级降序+名称），消除行跳动。
- **验证**：lcd dir/test/bench 全部纯净，切页/恢复无任何残留，
  面板实时数据仅 HOME 页显示，SYSTEM 排序稳定（用户确认完全正常）。
### 12.45 LCD 底层驱动性能榨干（fill 16 像素展开 + 字符串整窗连续写，v129）
- **lcd_fill**：16 像素循环展开（8 连写 ×2 组），消除逐像素循环判断开销；
  像素计数变量提升为 uint32，杜绝大区域/整屏填充的 16 位溢出隐患。
- **lcd_show_string**：整串**单窗口连续写**（行优先渲染，含 1px 间距背景列，
  支持多行/宽度裁剪），替代逐字符 SetWindow（每字符 3 次窗口写），
  字符串绘制理论提升约 5 倍。
- **安全边界**：FSMC 写时序保持已验证的 2/2（12ns，ST7789 最小写周期约束，
  1/1 有风险不采用——性能让位于长期可靠性）。
- **字体布局核对**：asc2_1206=6 列×2B、asc2_1608=8 列×2B、asc2_2412=12 列×3B，
  行优先取 `bi=row>>3` 与官方 lcd_show_char 逐字节一致（无渲染错位风险）。
- **状态**：v129 已 OTA 上线；bench 实测待实机确认
  （对比基线 v115：clear 20.6ms / char16 35µs / string 2.23ms/20 字）。
### 12.46 LCD UI 渲染任务框架（LcdUI，RTOS 极致应用层，v130）
- **架构**：新增 `SystemServices/lcd_ui.{c,h}`——独立 LcdUI 渲染任务
  （2048B 栈，osPriorityNormal）+ 8 深命令队列 + 消息结构；所有绘制异步提交、
  渲染任务串行执行，成为全局唯一写屏者，从根上杜绝并发写冲突与画面参杂。
- **命令集**：Clear / Fill / Text / Num / Bar(进度条) / Rect /
  ShowPage / NextPage / EnterTest / ExitTest / **RunTest**。
- **页面注册**：`LcdUI_AddPage({name, draw, refresh})` 最多 6 页；
  渲染任务空闲 1s 周期调用 refresh()（测试模式暂停）。
- **测试隔离**：shell `lcd test/bench/dir/clear` 全部改为 `LcdUI_RunTest`
  回调——测试画面在渲染任务内执行，与面板刷新完全互斥
  （根治历史上"界面互相参杂/数字残留"一类问题）。
- **lcd_app 重构**：HOME/SYSTEM/BUS 三页注册 + 按键 LcdUI_NextPage +
  任务固定排序；测试模式接口从 LcdApp_* 迁移至 LcdUI_*（lcd_app.h 精简）。
- **工程**：lcd_ui.c 已加入 MDK-ARM/APP.uvprojx（SystemServices 组）；
  编码按 .editorconfig UTF-8，shell.c（GBK）用 PowerShell GB2312 读写
  做外科式修改，未破坏原有中文注释。
- **验证**：编译 0 Error / 0 Warning；v130 (build 114) OTA 全链路成功
  （BOOT phase 2→7 全部 err=0）。实机面板/切页/1s 刷新/bench 待确认。
### 12.47 字体斜体根因（整串渲染窗口宽度 off-by-one）+ 修复（v131）
- **现象**：v130 上机后所有字体变斜（逐行左移的剪切效果）。
- **根因**：v129 整串渲染调用 `lcd_set_window(x, y, n*(cw+1)-1, size)`，
  但 lcd_set_window 的 width 参数是**像素个数**（内部 `x1=sx+width-1`），
  多减 1 使窗口比每行写入的 `n*(cw+1)` 像素窄 1px——每行末 1 像素溢出
  滚入下一行行首，逐行累积左移 1px，视觉上即"斜体"。
- **修复**：窗口宽度改传 `n*(cw+1)`，与写入流严格一致；已加注释防止回退。
  lcd_fill/lcd_show_char 的窗口调用本就是像素个数，无此问题。
- **验证**：编译 0 Error / 0 Warning；v131 (build 115) OTA 全链路成功。
  实机字体确认中。
### 12.48 页面切换残留（SYSTEM 标题/IDLE 行未清干净）+ 恢复逐字符渲染（v132）
- **现象**：v131 字体已恢复端正，但 SYSTEM → 其他页切换时 SYSTEM 标题
  与任务行（IDLE 116 0）残留，且多处页面可见。
- **排查**：逐项核对 lcd_fill/lcd_show_char/lcd_show_string 的窗口像素数，
  静态上 v131 写入计数全部与窗口一致；唯一与 v128（实机确认纯净）不同的
  渲染路径是 v129 的整串单窗口渲染，判定为最大嫌疑（实机交互问题静态难复现）。
- **处置**：lcd_show_string 恢复 v128 已验证的逐字符实现
  （每字符一次完整窗口，无跨行偏移/残留风险）；字符级性能优化
  （lcd_show_char 窗口连续写）与 lcd_fill 16 像素展开保留。
- **验证**：编译 0 Error / 0 Warning；v132 (build 116) OTA 全链路成功。
  实机切页/残留确认中。
### 12.49 页面残留 + 下半屏花屏根因（lcd_fill 16 像素展开破坏 GRAM）+ 还原（v135）
- **现象演进**：v130-v134 期间，SYSTEM → 其他页切换残留任务行
  （IDLE 116 0）；v133 整屏清底后标题残留消失但 IDLE 行仍在；
  v134 实测**重新上电后下半屏完全花屏**。
- **排查**：全工程 grep 确认 "IDLE" 仅由 lcd_system_draw 绘制，且框架日志
  （draw/refresh）证明切页状态机完全正确（HOME 页只发生 draw/refresh HOME）；
  排除中断/钩子/其他任务画屏（SysTick 钩子仅喂狗、UART IDLE 中断仅断帧）。
- **定位**：v128（实机完美）与 v129+ 的驱动层差异只剩 **lcd_fill 的 16 像素
  循环展开**。展开后连续大区域写入（整屏清屏 76800px、SYSTEM 页多行长填充）
  突发过于密集，ST7789 在高速长填充下 GRAM 丢像素/错位 →
  下半屏花屏 + 特定区域像素残留（IDLE 行）。
- **处置**：lcd_fill 还原 v128 普通循环（逐像素写，写入节奏与已验证版本
  完全一致）；lcd.c 现已与 v128 完全等价（仅注释差异）。
- **验证**：编译 0 Error / 0 Warning；v135 (build 119) OTA 全链路成功。
  实机重新上电/切页/残留确认中。
### 12.50 HOME/BUS 布局对齐规范化 + SYSTEM 1s 刷新去频闪（v136）
- **布局规范**（HOME/BUS 统一）：标签左列 x=8（FONT12），数值右对齐
  x=150（FONT16），行距 26px；数值上移 2px 与标签垂直居中
  （FONT16 中心线与 FONT12 中心线重合），右对齐后数字/单位尾字符对齐
  成一列，观感整齐；HOME 标题与信息区加 1px 分隔线。
- **页脚修复**：原页脚提示文字 y=h-11（FONT12 越界 1px 被
  lcd_show_char 越界保护跳过，从未显示）→ 页脚改高 14px、
  文字置于 y=h-13，提示可见。
- **SYSTEM 去频闪**：1s 刷新由"整页重绘"（清内容+页眉+列表，每 1s
  全屏闪黑）改为**只重画任务列表**（每行先清后画），页眉/页脚/分隔线
  保持不动——恢复 v128 验证过的无频闪路径。
- **清理**：移除 lcd_ui.c 诊断日志（draw/refresh/page），框架保持纯净；
  保留整屏清底 + 40ms 防撕裂（v133 起验证有效）。
- **验证**：编译 0 Error / 0 Warning；v136 (build 120) OTA 全链路成功。
  实机对齐/频闪确认中。
### 12.51 CPU 采样稳定性修复 + 内存优化（v137）
- **CPU% 每次切页不同**——定位为采样问题：原实现只在 HOME 页调用，
  差值窗口随切页时机变化（快速切页只有零点几秒且含绘制突发负载）；
  且运行时间统计用 DWT 168MHz（32 位 25.6s 回绕），长间隔会算错。
  **修复**：三页 refresh 均调用 lcd_update_cpu → 采样节奏恒定 1s；
  短间隔（<900ms）沿用上次值；超长间隔（>5s）仅重建基线防回绕。
  现在 CPU% 是稳定的 1s 窗口读数，与切页时机无关。
- **内存优化**：
  1. 任务状态缓冲 `s_lcd_tasks[16]` 静态化——消除每秒 640B
     pvPortMalloc/vPortFree 的堆抖动与碎片（仅 LcdUI 任务串行使用）；
  2. LcdUI 任务栈 4096 → 2048B（诊断期临时放大，根因已定为填充展开，
     v128 同负载在 1536B 任务内验证过）；
  3. FreeRTOS 堆 30720 → 35840B（RAM 128KB 内安全：ZI 91.76KB）。
- **验证**：编译 0 Error / 0 Warning；v137 (build 121) OTA 全链路成功。
  实机 CPU 稳定性 / Heap 余量确认中。
### 12.52 LCD 自动化测试基础设施 + 框架清理（v138）
- **自动化测试模块** `SystemServices/lcd_test.{c,h}`（shell: `lcd selftest` /
  `lcd soak <sec>` / `lcd stress <n>`）：
  - **SelfTest**：GRAM 写-读回像素级校验（FSMC 读时序 360ns 可靠）——
    全屏 5 色网格采样、窗口边界、1px 窗口隔离、字符/字符串
    128/272 像素逐点对照字体点阵（可捕获斜体/残留/错位类回归）；
  - **Soak**：混合负载长稳（清屏+色块+字符串+数字循环），每轮读回校验
    + 堆稳定性报告；
  - **Stress**：快速切页命令注入 + 队列排空/丢命令统计 + 堆报告。
- **驱动**：新增 `lcd_read_point_rgb565()`（ST7789 单次 16 位读回）。
- **框架清理**：移除切页冗余的整屏清底（v133 为残留打的补丁，根因
  fill 展开已修复，页面绘制本就覆盖全屏），切页更快、无黑闪；
  新增 `LcdUI_GetPendingCount/GetDroppedCount`，`LcdUI_NextPage` 返回状态。
- **验证**：编译 0 Error / 0 Warning；v138 (build 122) OTA 全链路成功。
  实机 selftest/soak/stress/bench 结果待确认。
### 12.53 LCD 全面自动化测试报告（v138/v139）
- **读回校准修正（v139）**：lcd_read_point_rgb565 改用官方双读 + RGB565
  还原公式——校准表 写入==读回 完全一致（RED 0xF800/GREEN 0x07E0/
  BLUE 0x001F/WHITE 0xFFFF），测试改为直接比对，校验更严格。
- **SelfTest（654 项，285ms）**：PASS
  - fill：5 色全屏 240 点网格采样 PASS
  - window：50x50 蓝窗内/外 7 点边界 PASS
  - 1px-window：单点红+四邻蓝 5 点隔离 PASS
  - char：'A' 128 像素逐点对照字体点阵 + 窗外 2 点 PASS
  - string："AB" 272 像素（含 1px 间距列）逐点对照 PASS
- **Soak（30s）**：PASS —— 775 轮混合负载，1550 次读回校验 0 错，
  堆 13256 → 13256（+0）零泄漏
- **Stress（50 条）**：PASS —— 快速注入下队列满按设计丢弃 41 条
  （人按按键速率远达不到），队列干净排空，堆 +0，无崩溃
- **Bench**：char16 35µs / string 2.23ms（与基线一致）；
  clear 30.3ms / fill 1.98ms（2.5 MPix/s，v128 正确性优先的普通循环速率）
- **遗留**：切页/无残留/SYSTEM 无频闪 目视确认（自动化无法覆盖）。
### 12.54 触摸屏系统（XPT2046 电阻屏，极致丝滑架构，v140）
- **硬件**：探索者V3 LCD 触摸接口——T_CLK=PB0 / T_PEN=PB1 /
  T_MISO=PB2 / T_CS=PC13 / T_MOSI=PF11（官方 V3 例程确认，全空闲无冲突）。
- **分层架构**：
  - `BSP/bsp_touch.{c,h}`：XPT2046 位操作 SPI（DWT 半时钟 ~1.6MHz），
    5 次采样去极值均值 + 双次校验（±50），SPI 互斥锁（采样/校准串行化），
    线性校准模型（逻辑 = (物理-中心)/比例 + 屏/2）；
  - `SystemServices/touch_svc.{c,h}`：独立 TouchSvc 采样任务
    （空闲 100Hz PEN 轮询，按下 1kHz 采样），状态机 UP/DOWN/MOVE/UP +
    轻平滑（(新+3旧)>>2），共享状态 + 代数计数器（UI 轮询无队列洪泛）；
  - `SystemServices/lcd_ui`：触摸期间 8ms 快速轮询（空闲 1s 刷新暂停），
    8x8 白色光标实时跟随手指，横向滑动 >40px 切页，tap → 页面触摸回调
    （页面可注册自定义触摸反馈）；
  - `Application/lcd_app`：新增 TOUCH 测试页（实时位置/原始 AD/状态/
    校准参数 + 触摸回调实时刷新）。
- **校准**：`touch cal` 四角校准（TL/TR/BR/BL 逐点按压，轴对齐线性映射，
  支持各轴反向）；内存态 + 出厂默认近似值（持久化待分区方案，日志备注）。
- **shell**：`touch info|cal|test`。
- **验证**：编译 0 Error / 0 Warning；v140 (build 124) OTA 全链路成功；
  系统 11 任务/堆 12.3KB 正常。实机触摸手感/校准/丝滑度待用户确认。
### 12.55 触摸三项实机问题修复（v141）
- **校准画面不纯净**：校准期间 1s 页面刷新仍在重绘（HOME 实时值覆盖十字）。
  **修复**：TouchSvc_Calibrate 首尾加 LcdUI_EnterTest/ExitTest（暂停周期刷新），
  且 UI 测试模式下抑制光标绘制——校准画面只显示十字+提示。
- **轻触不识别（需使劲按）**：原方案依赖 PEN 引脚（XPT2046 内部阈值高，
  轻触不触发）。**修复**：改为**按 XY 采样值判定触点**——无触摸时
  probe 实测 raw=(0,4095)（超范围），触点有效值在中段（20~4075）；
  BSP_Touch_ReadRaw 先做单轴 X 快速探测（超范围立即返回，省 3/4 开销），
  服务空闲 8ms 轮询 + 连续 2 次有效消抖；双次校验 ±50 放宽到 ±80
  （轻触噪声略大）。PEN 不再参与检测。
- **滑动后页面停不下来**：状态机 bug——抬起后 UP 事件在空闲循环里被
  重复置位，UI 每 10ms 又触发一次滑动切页。**修复**：UP 只发一次，
  下一拍复位 NONE（不产生新事件）；UP 后再次触摸正确判定为 DOWN。
- **验证**：编译 0 Error / 0 Warning；v141 (build 125) OTA 全链路成功；
  空闲 probe raw=(0,4095) 验证判定范围正确。实机轻触/滑动/校准待确认。
### 12.56 触摸滑动丢失根因（UP 竞态）+ 灵敏度提升 + UI 轮询重构（v142）
- **滑动不切页根因**：v141 把 UP 后状态复位为 NONE 且不递增代数——
  服务在 8ms 空闲周期内复位，UI 轮询周期同为 8ms，**UP 事件常被 UI
  看到前就丢失**，手势永不执行（v140 相反：UP 反复重发导致连切）。
  **修复**：UP 状态保持到下一次按下（UI 按代数消费，不重复不丢失）。
- **UI 轮询重构**：原"触摸中 8ms / 空闲 1s"条件切换在快速再次触摸时
  会阻塞最多 1s。改为**固定 8ms 节拍** + 1s 时间闸门刷新——
  任何触摸（含连续点击）延迟恒 ≤8ms，刷新节奏不变。
- **灵敏度提升**：SPI 1.6MHz → 1MHz（弱接触高源阻抗下采样保持电容
  获取时间更长，轻触读数更稳）；检测路径去掉双次校验（轻触抖动
  ±80 内易误拒），改单次 X 快速探测 + X/Y 中值 + 触点范围判定，
  噪声由服务层轻平滑吸收。采样 500Hz 兼顾系统负载。
- **物理说明**：电阻屏必须施加压力使两层导电膜接触，属物理原理；
  若屏面覆层偏硬，所需力度更大——建议使用触笔（集中压强）。
- **验证**：编译 0 Error / 0 Warning；v142 (build 126) OTA 全链路成功。
  实机轻触/单次滑动/校准待确认。
### 12.57 光标覆盖层（读回-恢复）+ 手势最大位移判定（v143）
- **光标闪烁/拖影根因**：擦除用"填黑"，光标划过蓝/灰/彩色区域留下
  黑洞与拖影，叠加位置抖动 → 闪烁。**修复**：光标改为**真覆盖层**——
  绘制前 BSP_LCD_ReadPixels 读回 8x8 原色备份，擦除时
  BSP_LCD_WritePixels 逐像素原样恢复（无黑洞无拖影）；2px 位移死区
  吸收亚像素抖动（不重绘）；服务平滑增强 (新+7旧)>>3。
- **滑动不切换根因**：轻触接触不稳定，滑动途中触点瞬断提前触发 UP、
  起终点位移不足阈值。**修复**：服务跟踪**触摸全程最大位移**
  （max_dx/max_dy），UP 时以全程最大位移判定——接触抖动/中途瞬断
  不再影响手势。
- **新增 BSP 批量像素接口**：BSP_LCD_ReadPixels / WritePixels
  （读回恢复 + 未来图像/光标层通用）。
- **验证**：编译 0 Error / 0 Warning；v143 (build 127) OTA 全链路成功。
  实机光标丝滑度/单次滑动待确认。
### 12.58 滑动失效真根因（事件机死锁）+ 世界级抗噪算法包（v144）
- **真根因（关键）**：touch_svc 事件判定误写为
  `(state==MOVE)?MOVE:DOWN`——DOWN 后状态恒为 DOWN，**永远产生
  DOWN、永不转移为 MOVE**。后果：max_dx/max_dy 每次被 DOWN 分支
  清零（滑动判定永不成立→不切页）；平滑仅作用于 MOVE 分支从未生效，
  光标跟随未滤波原始值（→剧烈抖动漂移）。**修复**：事件转移改为
  NONE/UP → DOWN，其余 → MOVE。
- **世界级抗噪算法包**：
  1. **物理速度钳制**：单采样位移限 ±20px（人手最快 ~12px/采样@500Hz，
     噪声尖峰可达 100px+，超限按方向钳制）；
  2. 轻平滑 (新+7旧)>>3（仅 MOVE，降漂移）；
  3. 2px 位移死区 + 读回-恢复光标覆盖层（无黑洞/拖影）；
  4. 检测范围放宽 (5~4090) + 3 次中值快速探测（更轻触可识别）。
- **诊断**：UP 时日志 `[LCD] touch: swipe/tap max=(dx,dy)` 验证手势。
- **验证**：编译 0 Error / 0 Warning；v144 (build 128) OTA 全链路成功。
  实机滑动/光标/轻触待确认。
### 12.59 触点偏移处理：四角校准 + 像素微调（v145）
- **偏移原因**：出厂默认校准（xfac=18/yfac=12/中心 2048）为近似值，
  与该屏实际电气映射有偏差；`touch cal` 四角校准数学已验证
  （模型 逻辑=(物理-中心)/比例+屏/2，四点平均，支持各轴反向）。
- **新增 `touch nudge <dx> <dy>`**：校准后残余偏移按像素直接微调
  （xc += dx*xfac, yc += dy*yfac），无需重跑校准。
- **验证**：编译 0 Error / 0 Warning；v145 (build 129) OTA 全链路成功。
  实机校准精度待确认。
### 12.60 触摸系统收官（v146，生产干净版）
- 用户实机确认：轻触灵敏、光标丝滑跟手无闪烁漂移、滑动精确切页、
  校准后触点与光标零偏移——**触摸系统达标收官**。
- v146 移除验证期手势诊断日志（swipe/tap），生产版本纯净；
  编译 0 Error / 0 Warning，OTA 全链路成功。
- **遗留备忘**：校准值目前内存态（重启回默认近似值）；持久化需新增
  独立存储分区或启用板上 EEPROM（AT24C02），待后续任务。
### 12.62 IMU/MPU6050 系统（I2C1 + Mahony AHRS + 200Hz 数据管线，v148/v149）
- **硬件**：外接 MPU6050 → I2C1（PB6=SCL/PB7=SDA，HAL 400kHz）。
- **分层架构**：
  - `Core/Src/i2c.c`：I2C1 硬件初始化；
  - `BSP/bsp_mpu6050.{c,h}`：寄存器驱动（WHO_AM_I 0x68/0x69 探测、
    ±250dps/±2g 最高分辨率、200Hz 采样、DLPF 98Hz、14 字节突发读、
    零偏校准（含水平姿态守卫）、I2C 互斥锁 + 总线错误自恢复重试）；
  - `SystemServices/imu_fusion.{c,h}`：**Mahony 四元数 AHRS**
    （Kp=0.5 加速度信任、Ki=0.05 在线零偏漂移补偿；选型说明：
    六轴无磁力计场景 Mahony 为工业标准——2D 卡尔曼只解两轴且调参脆弱，
    完整 EKF 需 9 轴+大算力，Mahony 是精度/鲁棒/算力三重最优；
    航向仅陀螺积分会缓慢漂移，属六轴物理极限）；
  - `SystemServices/imu_svc.{c,h}`：200Hz 采样任务 + 共享状态
    （四元数/欧拉角/加速度/角速度/温度/采样与故障计数）+ 在线重校准；
  - `lcd_app`：新增 **IMU 页**（人工水平仪 + 实时 R/P/Y/A/G/T，
    50ms 页面级刷新——LcdUI 新增 refresh_ms 页面周期机制）；
  - shell：`mpu info|test|cal`。
- **-O2 工程优化（重要）**：v148 体积 122KB 超 OTA 下载区上限
  （112KB）被拒 → 目标优化 -O1→-O2，bin 降至 101KB；但 -O2 使
  lcd.c 填充循环过度紧凑、写入突发超 ST7789 极限导致 GRAM 丢像素
  （selftest 复现 97 项失败）→ **lcd.c 单独 -O1** 修复，
  selftest 654 项全过。教训：FSMC 直写循环必须保留写入间隔，
  已用自动化 selftest 捕获该回归。
- **验证**：MPU ready、3317 采样、0 故障；静止 R=-0.93° P=-1.24°
  Y=+0.06°（水平收敛正确）、陀螺零偏 ~0 dps、温度 30.3℃；
  LCD selftest 全过、触摸空闲探测正常。实机运动跟踪/水平仪/触摸
  回归待用户确认。
### 12.63 IMU 页 3D 姿态立方体 + 物理单位（v150）
- **单位标注**：横滚/俯仰/航向加 `deg`，加速度加 `g`，角速度加 `dps`，
  温度加 `C`（ASCII 字库无 ° 符号，用 deg 表达）。
- **3D 姿态立方体**（替代 2D 水平仪，20Hz 实时渲染）：
  - 四元数旋转 8 顶点（q⊗v⊗q* 优化公式）+ 正交投影；
  - **可见面消隐**：旋转各面法线，仅绘制朝向观察者的 3 个面；
  - **凸四边形扫描线填充**（逐行 1px 窗口，复用 LCD 高速填充）；
  - 配色 X=红 / Y=绿 / Z=蓝（工业 IMU 标准），正负方向亮度区分，
    白边勾勒轮廓增强立体感；
  - 擦旧画新：旧四元数渲染为黑擦除，无残影。
- **验证**：编译 0 Error / 0 Warning；bin 107.7KB（OTA 内）；
  v150 (build 134) OTA 全链路成功；selftest 654 项全过、MPU 0 故障。
  实机立方体旋转/单位显示待用户确认。
### 12.64 IMU 3D 渲染重构：线框立方体 + 中心坐标轴（v151）
- **用户反馈问题**：旋转超一定角度界面卡死/白屏；填充渲染频闪难看。
- **重构（对应全部诉求）**：
  1. **纯线框**：移除凸多边形扫描线填充（卡死/白屏的最大嫌疑——
     异常姿态下填充路径失控），只画可见边（隐藏边消隐，立体感正确）；
  2. **去频闪**：线框每帧仅 ~0.5ms（原填充 ~15ms），20Hz 刷新无闪烁；
  3. **体积缩小**：半边长 40 → 28；
  4. **中心三维坐标轴**：X红/Y绿/Z蓝，相对立方体静止（随姿态旋转），
     长度 36 超出立方体便于观察；
  5. **坐标钳制**：所有画线前边界钳制（0..239 / 0..319），
     杜绝越界坐标导致画线循环失控；
  6. **UI 任务栈** 2048 → 2560B（渲染路径余量）。
- **验证**：编译 0 Error / 0 Warning；bin 107.7KB；v151 (build 135)
  OTA 全链路成功；selftest 654 项全过、MPU 0 故障。
  实机旋转稳定性/无频闪/坐标轴方向待用户确认。
### 12.65 IMU 五项升级：卡尔曼滤波/箭头轴/去频闪/定宽单位/偏航漂移抑制（v152）
- **卡尔曼滤波**：Mahony 输出叠加**一维卡尔曼**（R/P/Y 各一轴）——
  陀螺做预测（角度 += 角速度·dt，协方差随时间累积）、Mahony 角度做
  测量修正；Q=0.1/R=1.0 兼顾平滑与响应，输出噪声显著下降。
- **坐标轴箭头**：三轴尖端加 V 形箭头（屏幕空间垂直向量 + 两翼短线）。
- **去频闪**：刷新 20Hz → 40Hz；**姿态变化 <0.5° 不重绘**（静止零闪烁）；
  线框每帧 ~0.5ms，快速旋转时擦旧画新间隔极小，肉眼不可见。
- **G 行单位乱修复（根因）**：字符串长度随数值变化，旧文字残留叠加成
  "dps dpsss"。改为**全部定宽格式**（%+6.1f/%+5.2f/%10lu 等），
  长度恒定，从根上消除残留。
- **偏航漂移抑制**：静止检测（|a|≈1g 且角速度小）连续 ~0.5s →
  在线估计 gz 零漂并扣除（融合输入与显示同步修正）；静止时偏航
  停止漂移，运动时按真实角速度积分。Mahony Ki 已补偿横滚/俯仰漂移，
  此为对偏航（无磁力计绝对参考）的工程最优解。
- **R/P 正负修正**：q1/q2 镜像取反（q'=(q0,-q1,-q2,q3)），数值与
  立方体一致反转，偏航不变。
- **验证**：编译 0 Error / 0 Warning；bin 108.4KB（OTA 内）；
  v152 (build 136) OTA 全链路成功；selftest 654 项全过、MPU 0 故障；
  静止 P 由 -3.59° → +3.12°（镜像生效）。实机手感待用户确认。
### 12.66 IMU 卡尔曼最大化重构 + R/P 轴对调 + A/G/T 列对齐（v153）
- **卡尔曼最大化（核心重构）**：从"Mahony 输出后置平滑"升级为
  **经典 2D 卡尔曼直接融合**——每个倾角轴双状态（角度 + 陀螺零偏）：
  陀螺预测（角速度积分 + 协方差传播）、加速度计直接测量
  （atan2 重力方向）、最优增益更新；**零偏运行中在线估计**（优于
  静态校准），Q_angle=0.001 / Q_bias=0.003 / R_measure=0.03
  （rad 域经典调参）。偏航为陀螺积分 + 静止漂移抑制（无磁力计
  绝对参考下的最优解）。
- **R/P 轴对调**：传感器安装轴向导致物理横滚被读成俯仰——
  数据入口 **X/Y 轴对调**（陀螺+加速度同步），数值与立方体
  一致修正；立方体四元数由 欧拉(卡尔曼R/P + 积分Y) 重建，
  与显示完全一致。
- **A/G/T 列对齐**：三行数值统一 6 字符定宽（%+6.2f / %+6.1f），
  三列精确对齐且无残留。
- **验证**：编译 0 Error / 0 Warning；bin 108.7KB（OTA 内）；
  v153 (build 137) OTA 全链路成功；selftest 654 项全过、MPU 0 故障。
  实机 R/P 方向/卡尔曼平滑度待用户确认。
### 12.67 IMU 实时性优化：运动自适应卡尔曼 + 立方体 P 方向修正（v154）
- **RP 延迟根因**：静态卡尔曼调参（Q_angle=0.001）在运动时过度
  平滑、响应滞后。**修复：运动自适应测量噪声**——
  R = 0.02 + 0.3·|角速度|：静止时低 R（信任加速度计，收敛稳定），
  运动时 R 随角速度增长（信任陀螺预测，实时跟随无滞后）；
  Q_angle 0.001→0.01、Q_bias 0.003→0.0003（零偏慢变，防吸收运动）。
- **立方体 P 方向**：欧拉→四元数时 pitch 取反，立方体俯仰与
  P 角同向。
- **T 行**：n= 计数改为紧凑格式（%lu，计数单调递增无残留），
  数字紧贴等号。
- **立方体重绘阈值** 0.5° → 0.3°（更实时）。
- **验证**：编译 0 Error / 0 Warning；bin 108.7KB；v154 (build 138)
  OTA 全链路成功；selftest 654 项全过、MPU 0 故障。
  实机实时跟随/P 方向待用户确认。
### 12.68 IMU 极致实时：100Hz 显示 + 更猛的运动自适应（v155）
- **估计侧**：卡尔曼运动自适应加强——Q_angle 0.01→0.02、
  R_MOTION 0.3→0.5（运动中输出≈纯陀螺积分，零滞后）；
  Q_bias 0.0003→0.0002（零偏更慢变，防吸收运动）。
- **显示侧（关键）**：IMU 页刷新 40Hz → **100Hz**（10ms 延迟，
  肉眼不可感知）；A/G/T 状态行降为 10Hz（诊断数据，快慢路径分离，
  渲染成本可控）；立方体重绘阈值 0.3°→0.2°。
- **验证**：编译 0 Error / 0 Warning；bin 108.7KB；v155 (build 139)
  OTA 全链路成功。实机实时跟随手感待用户确认。
### 12.69 串口资源重规划（ETH 前置）：调试口 USART2 → USART3（v156）
- **背景**：ETH 需占用 PA2（MDIO），USART2 在 F407 上固定 PA2/PA3
  不可复用 → 调试口必须迁移。
- **最终串口分配**（结合引脚复用，用户确认 SD 引脚可外接）：
  - USART1 (PA9/PA10)：HOSTLINK（不动）；
  - **USART3 (PC10/PC11)**：调试/Shell（115200，DMA1_S1_CH4 RX +
    DMA1_S3_CH4 TX，IRQ 已配）；USART2 退役（PA2 让给 ETH）；
  - UART5 (PC12/PD2)：摄像头（预留，未初始化）；
  - USART6 (PC6/PC7)：ESP32-S3（预留，信号发生器 UART 模式与
    LA PWM 源让位——次要功能）；
  - UART4 (PA0/PA1)：牺牲给 ETH（复用点 PC10/11 与调试口撞车）。
- **验证**：编译 0 Error / 0 Warning；bin 109.1KB（OTA 内）；
  v156 (build 140) 构建就绪，待串口重连后烧录；
  烧录后调试杜邦线需从 PA2/PA3 移至 PC10/PC11。
  **实机验证**：用户移线至 PC10/PC11 后 OTA 烧录 v156 成功；
  COM9（USART3）正常响应 info/mpu info（12 任务/堆 10.5KB/IMU 0 故障），
  PA2/PA3 已释放待 ETH。APP.ioc 已同步追加 USART3 配置
  （CubeMX 模型源；全量重生成会覆盖自定义引脚，故采用模型同步+选择性合并）。

### 12.70 ETH/LWIP 集成 + CubeMX 再生成配置找回（v157/v158）
- **背景**：用户在 CubeMX 中生成 ETH(RMII)+LWIP 2.1.2+USER_PHY（生成于
  LWIP/App、LWIP/Target、Middlewares/Third_Party/LwIP），并重写了
  gpio/tim/usart/main/it/dma/fsmc/freertos/uvprojx。生成前已做安全备份
  `backup_pre_eth/`（本轮全面核对并恢复完毕后，按用户要求删除）。
- **被 MX 抹掉/改坏并已修复的配置（逐项核对 backup_pre_eth 得出）**：
  1. `gpio.c`：恢复 LA 采样 PG6/7/8/15 的 EXTI 配置；移除 CubeMX 误配的
     PB0-7 EXTI（PB6/PB7 是 I2C1=MPU、PB0/1/2 是触摸屏引脚，GPXTI 会抢引脚）；
     保留 PHY_RESET(PD3)。
  2. `stm32f4xx_it.c`：删除 MX 生成的 5 个默认 `__weak` fault C 处理器
     （与 err_mgr 的汇编异常入口同名冲突 #247，汇编版才是真正入口）；
     恢复被删的 DMA2_Stream6_IRQHandler（信号发生器 USART6 TX DMA）；
     EXTI9_5/EXTI15_10 收敛为 PG6/7/8/15；ETH_IRQHandler 保留。
  3. `main.c`：恢复 `MX_I2C1_Init()`（MPU6050 必需，MX 因 .ioc 无 I2C1 而丢失）；
     移除重复的 `MX_LWIP_Init()`（freertos.c 的 StartStartupTask 已调用一次，
     双调用会导致 tcpip_init/netif_add 重复初始化）。
  4. `stm32f4xx_hal_conf.h`：恢复 `HAL_I2C_MODULE_ENABLED`；补
     `ETHERNET_PHY_ADDRESS 0x00` / `PHY_TYPE YT8512C`（探索者V3 板载 PHY）。
  5. `FreeRTOSConfig.h`：恢复 `configUSE_TICK_HOOK=1`（sysmon 任务看门狗/
     ERR_TickMs 依赖 tick hook）、`configTOTAL_HEAP_SIZE=35840`、
     `ERR_HandleAssert` 原型（消除 configASSERT 隐式声明警告）。
  6. `pinout.h`：`DEBUG_UART_IRQn` 残留 `USART2_IRQn` → `USART3_IRQn`。
  7. `stm32f4xx_hal_msp.c`：**补齐缺失的 `HAL_ETH_MspInit`**——MX 6.18 +
     USER_PHY 未生成该函数，导致 ETH 时钟/RMII GPIO/PHY 复位/ETH 中断
     全部未初始化（ETH 即使编译通过也是死的）。置于 USER CODE 区防再生成清除。
  8. `sys_arch.c`：`UINT16_MAX` → `0xFFFFu`（ARMCC5 的 stdint.h 未提供该宏）。
  9. `la_sample.c`：补 `#include "dma.h"`（hdma_tim1_up 声明迁移至 dma.h）。
- **.ioc 根治（让 CubeMX 永久认识这些配置，杜绝再次被抹）**：
  - 新增 `I2C1`（400kHz，PB6=SCL/PB7=SDA）到 Mcu.IP/functionlistsort，
    MX_I2C1_Init 由 CubeMX 自动生成；
  - 新增 LA 引脚 `PG6/7/8/15 = GPXTI6/7/8/15` + `NVIC.EXTI15_10_IRQn`
    （原 .ioc 完全没有 LA 引脚，重生成必丢）；
  - 移除 PB0-7 的 GPXTI 误配：PB0=GPIO_Output（触摸 CLK）、PB1/2=GPIO_Input
    （触摸 PEN/MISO）、PB3/4/5=GPIO_Input 上拉（闲置）、PB6/7=I2C1；
  - FreeRTOS：任务栈恢复经实机验证值（startup 256/shell 384/logger 128/
    eventBus 384 words）、堆 35840、`configUSE_TICK_HOOK=1` 入模型。
- **新增 `Script/fix_after_mx.ps1`（幂等）**：CubeMX 每次再生成后运行一次，
  自动恢复以上全部“模型外”配置（fault 处理器去重、LA EXTI、DMA2_Stream6、
  MX_I2C1_Init、tick hook/堆、HAL_ETH_MspInit、dma.h、USART3 IRQ），
  把“重新生成→手动补丁”固化为“重新生成→跑脚本→编译”。
- **验证**：编译 **0 Error / 0 Warning**；APP.bin = 165.9KB。
- **⚠️ 新风险（架构级，待决策）**：
  1. **OTA 下载区上限 112KB（DOWNLOAD 128KB - 会话区 16KB）已无法容纳
     ETH 固件（165.9KB）**：需二选一——(a) 重排 Flash 分区（如 BACKUP
     384KB→256KB、DOWNLOAD 128KB→256KB，同时改 BOOT/APP/HOST 三端地址），
     或 (b) 精简 LWIP 功能把 bin 压回 112KB 内；
  2. **RAM 余量仅 ~3.4KB**（ZI 126.5KB / 128KB SRAM，未用 CCM 64KB），
     后续新增功能前需专项优化（如 LA 32KB IRAM 缓冲外置、LWIP 收包池 12→8）。
- **清理**：删除 APP 根目录历史 build_*.log（构建产物）；`backup_pre_eth/`
  在全部核对与编译验证后删除（恢复源为 git HEAD + 本日志）。

### 12.71 Flash 分区重排（方案a）+ RAM 整体瘦身（v159/v160）
- **背景**：ETH/LWIP 加入后 APP.bin=165.9KB，超过原 OTA 上限 112KB
  （DOWNLOAD 128KB - 会话区 16KB）；LA 32KB 内部 IRAM 缓冲挤占堆空间。
  经用户确认执行方案 (a)：**BACKUP 384→256KB、DOWNLOAD 128→256KB**，
  并彻底移除 LA 内部 IRAM 缓冲。
- **新分区（与 BOOT/boot_config.h、APP.sct/.ld、Other/flash分区 严格一致）**：
  - BOOT    0x08000000  64KB   扇区0-3
  - RUN     0x08010000  320KB  扇区4-6   （APP，末尾 8B 魔数+版本）
  - BACKUP  0x08060000  256KB  扇区7-8   （回滚源，**独立尾部有效性**）
  - DOWNLOAD 0x080A0000 256KB  扇区9-10  （**固件包 ≤232KB**）
  - PARAM   0x080E0000  128KB  扇区11
- **BOOT 联动修改（boot_app.c）**：
  - `boot_check_app_valid` 按区取有效魔数偏移（RUN=APP_SIZE-8 /
    BACKUP=BACKUP_SIZE-8），SP/PC 仍以 RUN 链接地址为准；
  - 备份 RUN→BACKUP 与恢复 BACKUP→RUN 的拷贝长度改为 BACKUP_SIZE
    （原 APP_SIZE=320KB 会写穿 256KB 的 BACKUP 溢出到 DOWNLOAD），
    并互写尾部有效性（RUN 尾 ↔ BACKUP 尾 8 字节）；
  - YMODEM 路径新增下载区越界保护（文件过大/写入超界即取消）。
- **APP 联动修改（ota_agent.c / app_config.h）**：
  - `OTA_DOWNLOAD_ADDR 0x080C0000→0x080A0000`、`SIZE 128KB→256KB`；
  - 会话槽区 16KB/512槽 → **24KB/768槽**（覆盖 ≤184KB 固件，断点续传粒度保持 240B/槽）；
  - **`ota_flash_erase` 单扇区 → 按地址区间擦除**：DOWNLOAD 现跨扇区 9+10，
    只擦单扇区会导致写进未擦扇区编程失败（0→1 不可写）；
  - `OTA_DOWNLOAD_SAFE = 256KB - 24KB = 232KB`。
- **LA 瘦身（la_sample.c/h、la_config.h、shell.c）**：
  - 删除 `la_stream_iram[8192]`（32KB 内部 SRAM），DMA 流模式统一使用
    外部 SRAM（FSMC NE3，32768 点）；`la_dma_buf` 命令与
    `LA_Sample_SetDMABuffer/IsDMASram` 移除（冗余分支剔除）。
- **ETH 收包池瘦身**：`ETH_RX_BUFFER_CNT 12→8`（省 6KB SRAM，仍满足
  零拷贝 2× 描述符需求）。
- **验证**：
  - APP v159：编译 0 Error / 0 Warning；**ZI 126,568 → 87,528（省 ~38KB）**，
    RAM 余量 ~41.5KB；bin ≈ 165KB（< 232KB OTA 上限 ✓）；
  - BOOT v160：编译 0 Error / 0 Warning；BOOT.bin ≈ 32KB。
- **⚠️ 迁移注意**：分区地址已变，**必须同时重刷 BOOT + APP**（DAP 或
  BOOT0 全片擦除后烧录）；旧 BOOT/APP 组合与新分区不兼容（下载地址不同），
  不能跨版本 OTA 升级到新分区。

### 12.72 分区重排后实机全量验证（DAP + OTA 双通道，v160/v161）
- **烧录链路（新分区首装）**：
  1. DAP（Keil UV4 -f）烧录新 BOOT（返回码 0）；
  2. 旧 APP 发 `ota` 命令置 BKP 升级标志复位 → 新 BOOT 进入升级模式
     （YMODEM 控制台在 **USART1/COM13@115200**，物理可达）；
  3. YMODEM 发送 165,572B 安全包（v160/build 200）→ BOOT 校验/备份/
     擦除/解密写入/提交 → 复位 → 新 APP 启动。**DAP 与 OTA 双通道闭环**。
- **系统级验证（v161）**：
  - 15 任务全活（新增 LwIP：EthLink/tcpip_thread/EthIf），栈余量健康；
  - IWDG 1s 喂狗正常；任务看门狗无 stall；事件总线/数据链路 0 丢失；
  - CPU：IDLE 89% / ImuSvc 8%；Free heap 7208B；
  - 复位原因为 External pin reset（shell reset），无新崩溃（Crash seq 5
    为 BKP 保留的历史记录，早期崩溃注入测试遗留，非当前故障）。
- **外设验证**：
  - LCD：id=0x7789 240x320，bench 清屏 18.63MPix/s、填充 18.72MPix/s、
    字符 142,857 字/s；
  - 触摸：校准有效（xfac=18 yfac=12），探针 OK；
  - MPU6050：WHO_AM_I=0x68、I2C1 400kHz、samples=5959 **faults=0**、
    R/P/Y 正常、温度 27.7°C；
  - **LA：DMA 缓冲=外部 SRAM 32768pts 自检 PASS，100kHz 采集 240,361
    样本 0 溢出**（32KB IRAM 移除后实测）；时间戳模式 EXTI 6/7/8/15 全使能
    （顺带修复 la_start 诊断打印 12→8 的过时引脚）；
  - 蜂鸣器/LED 命令响应正常（待用户目视确认）；
  - eb_stress 压力模式 32/200 缓冲饱和为设计行为，稳态 0 丢失。
- **OTA 全链路（v160→v161）**：165,572B 下载 0 丢包，BOOT 状态帧
  phase 2→7（VERIFY→BACKUP→ERASE→WRITE→COMMIT→DONE）全部回传，
  启动确认（PENDING→NORMAL，last build 201）生效。
- **回滚自测（关键）**：`ota_rbtest` 置 PENDING+MAX 复位 → BOOT 检测超限 →
  **从新 BACKUP(256KB) 恢复 v160 成功**（拷贝 256KB + RUN 尾魔数/版本补齐），
  随后重新 OTA v161（build 202）恢复最新。
- **结论**：新分区（BACKUP 256KB/DOWNLOAD 256KB）、RAM 瘦身（LA 外部
  SRAM）、ETH+LwIP 运行时共存全部实机验证通过，可提交。

### 12.73 LCD SYSTEM 页：稳定排序 + 触摸滚动（v162）
- **问题**：任务列表按 `uxCurrentPriority` 排序，FreeRTOS 互斥量**优先级
  继承**会临时抬高持锁任务的当前优先级 → 顺序每秒跳动；
  且列表被 `n > 12` 截断，15 个任务有 3 个看不到。
- **修复**：
  1. 排序键改为 **`uxBasePriority`（基础优先级）降序 + 名称升序**——
     不受优先级继承影响，顺序恒定；PRIO 列同步显示基础优先级；
  2. **触摸纵向滚动**：框架在 TOUCH_EVT_UP 时检测纵向滑动（max_dy 占优
     且 >40px），向当前页回调投递新增的 `TOUCH_EVT_SWIPE_UP/DOWN`；
     SYSTEM 页响应滚动（上滑看靠后任务，下滑回看），渲染任务上下文
     直接重绘，无延迟；
  3. 可视 12 行 + 右侧比例滚动条（仅溢出时显示，LGRAY 轨道/白色滑块）；
  4. 滚动位置变化时整区清一次防残影；进入页面自动回到顶部；
     页脚提示 SWIPE UP/DOWN: SCROLL。
- **验证**：编译 0 Error / 0 Warning；v162 OTA 全链路成功（BOOT phase
  2→7 全绿）；系统健康（事件总线 0 丢失、IWDG 正常）。任务排序稳定性
  与触摸滚动待用户目视确认。

### 12.74 ETH 极致集成 + 内存整体优化（v163~v178）
- **内存优化（先行，v163）**：
  - FreeRTOS 堆(35,840B)+LA 预触发缓冲(6,144B) 移入 **CCM(64KB)**——
    堆分配仅用于任务栈/FIFO/队列（CPU-only，DMA 缓冲全部静态），安全；
    scatter/ld 新增 `.ccmram` 区（zero_init，避免 RW 镜像膨胀）；
  - LwIP 池调优：MEM 12KB（TCP 发送零拷贝）、PBUF_POOL 8、TCP_WND/SND_BUF
    8×MSS、TCP_SEG 64、ARP_QUEUE 16；`LWIP_RAW=1`、`LWIP_SOCKET=0`；
  - 效果：**SRAM 占用 88.7KB→53.6KB（省 ~35KB，余量 ~76KB）**，
    CCM 用 42KB；Flash 173.5KB（OTA 限 232KB 内）。
- **ETH 应用层（v163~v165）**：`EthApp` 模块（模块注册表 prio 65）：
  链路/IP/MAC/RX-TX 帧计数、变量注册（eth_link/eth_rx/eth_tx）；
  `net`/`net ping`/`net ip`/`net udp`/`net dbg` shell 命令；LCD **NET 页**；
  sysmon ETH 监控项；lwip.c 链路回调钩子；ethernetif 帧计数钩子。
- **ETH 硬件链路排障记录（v166~v178，全部实机定位）**：
  1. **RMII TX 引脚**：探索者V3 的 ETH_TX_EN/TXD0/TXD1 在 **PG11/13/14**，
     CubeMX 默认 PB11-13 不接 PHY → 数据面全断（MDIO 独立故链路仍协商）；
     .ioc + MspInit 修正；
  2. **HAL_ETH_Start→Start_IT**：零拷贝模板必须中断模式，否则 RX 永不触发回调；
  3. **CHECKSUM_GEN_ICMP=1**：F4 MAC 只卸载 IP/UDP/TCP，ICMP 校验和必须软件算；
  4. **raw 回调载荷偏移**：lwIP raw_input 在推进 IP 头之前调用回调——
     回显/ping 匹配必须自己 `pbuf_remove_header`（导致回显不匹配、
     板端 ping 永远超时的元凶）；
  5. **tcpip_thread 栈 1024→2048**：raw 回调（回显）在 tcpip 线程执行，
     1024B 会栈溢出（err 系统捕获 #6 Stack Overflow）；
  6. **ping raw 回调不能吞包**：非匹配 ICMP 必须 return 0 放行给协议栈，
     否则 icmp_input 收不到 echo 请求、板子不再回 ping；
  7. UDP 回显服务（端口 7777）用于双向 RTT/吞吐验证。
- **实机验证（板↔电脑 USB 网卡，静态 192.168.10.x）**：
  - 链路 100M 全双工（PHY st=2）；ARP 双向；帧校验和逐字节验算全对；
  - **UDP 回显 30/30（40B，RTT 1.44~2.90ms 均 1.69ms）+ 60/60（1400B，
    RTT 均 1.89ms，零丢包）**；PC 收到板端主动 UDP；
  - 板↔电脑 **ICMP 双向仍超时**：防火墙三配置文件全 OFF、无 WFP ICMP
    过滤、UDP 正常——判定为这台电脑系统级 ICMP 策略（GPO/安全中心/
    网卡驱动）问题，与固件无关（板端 ICMP 回复帧已逐字节验证合法）。
- **结论**：ETH（链路/收发/诊断/可视化）与内存优化全部落地并实证，
  可提交。后续 TCP/UDP 服务开发按需给电脑加放行规则即可。

### 12.75 综合性能与内存摸底优化（v181~v183）
- **摸底（map + 实机采集）**：
  - 静态 RAM 95.5KB = 堆 35.9KB(CCM) + ETH RX 池 13.2KB + LwIP MEM 12.3KB
    + MEMP 8.5KB + LA 预触发 6.2KB(CCM) + 事件总线 4.6KB + 启动栈 4KB + 其余；
  - 任务 15 个，栈占用 80~92%（ImuSvc 最紧 78B 余量），CPU：IDLE 89%、
    ImuSvc 8%、TouchSvc 1%，无饿死；空闲堆仅 4.5KB（紧张）；
  - **taskstats 输出乱码/截断**：根因 `LOG_Printf` 用 `vsnprintf` 返回值
    作为长度发送（超 256B 时栈越界读，输出堆垃圾）——潜在全局隐患。
- **优化（v181~v183）**：
  1. **FreeRTOS 堆 35,840→49,152 入 CCM**（CCM 64KB 用 59.8KB）：
     空闲堆 4.5KB→**17.3KB（4 倍）**，SRAM 零占用；
  2. **事件总线消息池移入 CCM**：SRAM 再省 4.6KB（纯 CPU 访问）；
  3. **logger.c 修复**：`vsnprintf` 长度钳制到缓冲内，杜绝栈越界读
     （所有长字符串打印受益）；
  4. **cmd_taskstats 重写**：堆分配 + 逐行打印（15 任务全量干净显示）；
  5. **shellTask 栈 1536→2048**（shell 命令含大局部缓冲，余量 110B 太险）；
  6. **EthIf 优先级 48→32**（低于事件总线：内联收包处理不得抢占系统事件）。
- **任务调度最终形态**：eventBus 48 / EthIf 32 / ImuSvc+TouchSvc 32 /
  logger 40 / shell+DataAgent+LcdUI+tcpip 24 / EthLink 16 /
  DL_TX+DL_CMD+WDOG 8 / Tmr 2 / IDLE 0——15 任务栈水位 78~543B 余量，
  CPU 峰值 ~11%，无反转/饿死风险。
- **验证**：编译 0 Error/0 Warning；v183 OTA 全链路绿；UDP 回显 30/30
  零丢包（RTT 1.44~2.40ms 均 1.66ms）；taskstats 15 任务全量显示；
  空闲堆 17.3KB；持续运行无崩溃。ETH 收包池 13.2KB 与 LwIP MEM 12.3KB
  为 DMA 必需（SRAM），保持不动。

### 12.76 TCP 服务（v184~v186）
- **交付**：工业 TCP 命令控制台（netconn API，端口 **9000**，行协议，
  最多 2 并发客户端，120s 空闲断开）：
  - 命令：help/info/ver/sysmon/taskstats/net/led/beep/mpu/echo/stream；
  - **stream on**：每秒推送遥测（UPTIME/HEAP/TASKS/ETH 链路与 IP）；
  - 新增 `TcpSvc` 模块（prio 66）、`tcp` shell 状态命令；
  - 任务：TcpSvc（服务，栈 2048）/ TcpCli（客户端处理，栈 2048，动态创建），
    taskstats 实测栈水位健康（TcpSvc 376 / TcpCli 230 余量）；
  - 配套 lwipopts：`LWIP_SO_RCVTIMEO=1`（netconn 接收超时）。
- **排障记录**：`netconn_set_recvtimeout` 受 LWIP_SO_RCVTIMEO 保护需显式
  开启；shell.c 字节级插入时表项锚点需用整行（前缀匹配会劈裂表项）。
- **验证（电脑↔板，TCP 出站不受 ICMP 影响）**：连接 9000 端口成功；
  ver/sysmon/net/echo/led/beep/mpu 全部返回正确；遥测流 1s 周期稳定
  推送、stream off 正常退出；任务增至 17（+TcpSvc+TcpCli），堆 12.6KB。
- **连接方式**：`python`/`nc`/`telnet 192.168.10.10 9000` 即可操作；
  后续 TCP 应用（HTTP/MQTT 等）按此框架扩展。

### 12.77 ETH 抓帧通道 + EthLab 上位机（v187~v191）
- **背景/问题**：初版上位机走 TCP 控制台 `net dbg all` 提取 TX/RX 帧，
  但 `EthApp_TxDbg/RxDbg` 的帧行实际打到**串口 LOG**，TCP 连接永远收不到
  帧 → 帧表恒为 0。根因：抓帧输出通道与解析通道错位。
- **固件侧（v191）**：新增**独立 UDP 抓帧通道（端口 7778）**：
  - `net cap on`：TCP 控制台把**对端 IP** 记为目标，此后每个 TX/RX
    帧封装为 UDP 发往 `对端:7778`（载荷 `dir(1) flags(1) orig_len(2,BE)
    raw[]`，dir=1 TX/2 RX，flags bit0=截断，>1468B 截断防 IP 分片）；
  - 实现要点：3 槽静态环 + `tcpip_callback` 投递（RX 钩子在 eth 输入任务、
    TX 钩子在 tcpip 线程，全部经 tcpip 线程发送保证线程安全）；
    `s_cap_sending` 标志防自抓环（抓帧包自身不进捕获流）；
    计数器 cap_sent/cap_drop 随 `net` 命令可查；
  - 钩子：RX `HAL_ETH_RxLinkCallback`（连续缓冲直拷）、TX
    `low_level_output`（`pbuf_copy_partial` 全链拷贝）；
  - 串口 shell 同步支持 `net cap on <ip>` / `net cap off`；
    固件 ZI +4.4KB（3×1472B 槽），编译 0 Error/0 Warning。
- **上位机侧（HOST/EthLab 重构，PyQt5）**：
  - **TCP 控制台**：命令/历史/快捷按钮/遥测流；连接后自动 `net cap on`；
  - **实时抓包**：UDP 7778 监听 + 帧列表（时间/方向/长度/源/目的/协议/
    摘要）、协议过滤、关键字搜索（含 hex）、暂停/自动滚动/上限、截断标注；
  - **逐字节协议结构图**（ByteView）：整帧按字段着色，Ctrl+滚轮缩放、
    悬停/点击看字段详情，配合字段树（偏移/长度/值/说明）与 Hex 视图；
  - **解码引擎**：Ethernet II/802.1Q VLAN/ARP/IPv4/IPv6/ICMP/TCP/UDP
    字段级解析 + IPv4/TCP/UDP/ICMP 校验和逐帧验算 + TCP 选项解析；
  - **统计**：按协议计数、TX/RX、校验和错误、帧率/吞吐实时曲线；
  - **捕获管理**：保存/加载 JSON、导出标准 PCAP（Wireshark 可开）、
    离线粘贴解析；**UDP 回显测试**（7777，成功率 + RTT min/avg/max）。
- **排障记录**：
  1. **帧行不上 TCP**：`LOG_Printf` 只发串口 → 定案独立 UDP 通道；
  2. **QThread 信号收不到**：`CaptureListener` 是 QThread，数据经队列
     信号投递，必须跑 Qt 事件循环（`processEvents`/`exec_`）才进面板；
  3. **L4 校验和误报 FAIL**：以太网短帧尾部有 padding（60B 下限），
     TCP/ICMP 校验和把 padding 算进去了 → 按 IP 总长度截取 L4 段修复，
     实机 TCP/UDP/ARP 33 帧校验错误归零；
  4. **OTA 反复 SEC_ERR_REPLAY**：板端 `last_build_no=229`，且 OTA 代理
     按 version+size 命中旧会话**续传**（下载区头部仍是 build 62 包）→
     校验永远失败；对策：换 v191 强制全新下载 + build 230 越过重放线；
  5. 历史遗留 `[CRASH] #7 shellTask Stack Overflow`（PC=0，记录不完整，
     非本次引入），待后续专项排查。
- **实机验证（板 192.168.10.50 ↔ 电脑 .201，v191 OTA 全流程
  phase 2→7 通过 + 启动确认）**：`net cap on` 指向对端正确；UDP 回显
  14/14 零丢包（RTT 1.00~2.54ms）；EthLab 实收 **33 帧**（16 TX/17 RX，
  含 1 ARP/4 TCP/28 UDP），UDP/TCP 校验和逐帧 OK；帧列表/字节结构图/
  字段树/统计/PCAP 导出全部联调通过；`net` 显示 CAP SENT/DROP 计数。
- **结论**：ETH 上位机（控制台 + 抓包 + 协议可视化 + 统计 + 测试工具）
  与固件抓帧通道端到端打通，本轮重大验证通过，可提交。

### 12.78 工程化整改：AI 工作流闭环 + 可复现性加固（v192 构建期）
- **背景**：综合评估发现三类系统性问题——单一事实源缺失（版本/分区多处散落）、
  可复现构建与测试未闭环（APP GCC 依赖未入库文件、CI 死配置、APP 主机单测无构建规则）、
  发布卫生（崩溃注入后门默认开启、3 个源文件编码损坏、README 魔数地址过时）。
- **解决**：
  1. **崩溃后门**：`app_config.h` 的 `CRASH_INJECT_ENABLE` 改为跟随 `APP_DEBUG_MODE`
     （发布构建自动为 0）；`self_check.ps1` 与 CI 新增对 `APP.bin` 的
     "Crash injection" 字符串扫描（Keil/GCC 产物实测均不含）。
  2. **编码归一**：`Config/pinout.h`（GBK）、`SystemServices/err_mgr.c`、
     `SystemServices/shell.c`（UTF-8/GBK 混合损坏）迁移为合法 UTF-8，
     并恢复 crash/eb_stress/la 帮助文本与关键注释；pre-commit 钩子
     （`.githooks/pre-commit`）强制暂存区 UTF-8 + LF + 末尾换行。
  3. **主机单测闭环**：`gen_cmake.py` 模板新增 `CMAKE_CROSSCOMPILING` 分支——
     交叉编译构建固件、纯主机构建构建 `host_tests`（协议/CRC/变量分片），
     `ctest` 可复现；`cmake/APP.ld` 与 `arm-none-eabi-toolchain.cmake` 入库。
  4. **单一事实源**：新增 `config/version.json`（版本/构建号），`common.ps1`
     自动读取；`OTA_Tool/config.json`、`version_lib.json` 为本地状态（含 UID/路径），
     移出 git 并加入忽略规则。
  5. **流水线增强**：`auto_pipeline.ps1` 修复 `-Skip*` 开关被模式覆盖的 bug；
     每阶段记录耗时（`*_sec`），报告附 BOOT/APP 产物 SHA256；
     `self_check.ps1` 新增编码/后门/版本一致性/可复现性/README 漂移检查。
  6. **CI 落地**：死配置 `APP/APP/.github/workflows/ci.yml` 删除，
     仓库根新增 `.github/workflows/ci.yml`（BOOT+APP GCC 构建、BOOT/APP ctest、
     HOST 测试、后门扫描、工作流文件版本化检查）。
- **验证**：APP Keil 发布构建 0 Error/0 Warning（APP.bin≈187.5KB）；APP GCC
  构建通过（text≈197KB < OTA 232KB 上限）；APP/BOOT ctest 全部通过；
  HOST VLink/LogicAnalyzer/EthLab 单测通过；`self_check.ps1` 全绿
  （除"工作流已入库"项，随提交消除）。
- **实机验证（Keil DAP + OTA，v191/build 232）**：STM32CubeProgrammer 不识别
  CMSIS-DAP 探针，改按指定路径——BOOT 用 Keil `UV4 -f`（DAP，返回码 0，
  -FO15 扇区擦除保留 APP）烧录；APP 用 HOSTLINK OTA（COM13 921600）从
  build 231→232 升级，BOOT phase 2→7 全部 err=0；复位后 COM9 日志确认
  `OTA : Agent ready (last build 232)`、17 模块初始化、ETH ready、
  无 HardFault/活动态 CRASH（仅历史 #7 恢复记录提醒）。
- **顺带修复**：`auto_ota.ps1` 失败判定正则把正常状态 `state=1` 误判为失败
  （false negative，实际升级成功）→ 改为 `err=[1-9]|state=[2-9]`；
  `auto_verify.ps1` 新增 `-SerialReset`（DAP-only 环境无 SWD 复位的替代路径），
  `com9_logger.py` 支持 `--cmd` 在打开串口后立即发送命令。
- **LA 硬件健壮性测试（不复测）**：`test_robustness.py` 为 60 组实机采样集成测试，
  需 PE2/PE3 接入逻辑分析仪通道；用户确认当前未接线且此前已实测过，
  本轮不复测（`auto_hosttest.ps1 -Robustness` 开关保留，供有接线环境使用）。

### 12.79 LCD NET 页排版修复（v191/build 233）
- **现象**：NET 页字符不齐，MAC 行与左侧标签重合覆盖。
- **根因**：数值统一右对齐到全局 `LCD_VAL_RIGHT=150`；MAC 为 17 字符 ×
  FONT16 8px = 136px，右对齐后起点 x=14，直接压到左侧 "MAC" 标签
  （x=8..26）上；其余行数值左缘也随长度参差。
- **解决**：NET 页新增 `lcd_net_val()`——数值统一右对齐到右边缘
  （240px 屏 → x=236，MAC 起点 100，安全间距），各行右缘整齐；
  12px 标签上移 2px 与 16px 数值垂直居中（Link 34/IP 90/MAC 116/
  RX 142/TX 168/Uptime 194）。
- **验证**：Keil 发布构建 0 Error/0 Warning；HOSTLINK OTA build 232→233
  全流程通过（BOOT phase 2→7 err=0），复位日志确认 `last build 233`、
  17 模块初始化、ETH ready、无活动态 CRASH；视觉效果待用户目检确认。

### 12.80 LCD NET 页右对齐宽度修正（build 234）
- **现象**：12.79 修复后 MAC 尾部仍被屏幕右缘裁掉（最后 1~2 字符）。
- **根因**：`lcd_val_at` 按 8px/字符估算右对齐起点，但 `lcd_show_string`
  实际步进为 `size/2 + 1 = 9px/字符`（含 1px 间距，见 lcd.c:1227）；
  MAC 17 字符实际宽 153px，起点 x=100 时右缘 253 > 240px 屏宽 → 裁切。
- **解决**：`lcd_val_at` 改为 `xr - n * 9u`，右对齐精确到最后一字右缘
  （MAC 起点 x=83、右缘 235，完整显示）；全页数值列随之精确右对齐。
- **验证**：Keil 0 Error/0 Warning；HOSTLINK OTA build 233→234 通过；
  复位日志 `last build 234` / 17 模块 / ETH ready / 无活动态 CRASH；
  视觉效果待用户目检确认。

### 12.81 Shell 架构重构：上层一致 + 底层物理协议可插拔（build 235）
- **背景**：命令注册表（cmd_shell）已有，但传输适配层隐式、命令目录与
  UART 适配器耦合在同一文件、CAN 扩展点不明确。
- **架构**：
  - `cmd_shell.h/c`（命令核心）：注册表/分发/会话上下文/LOG 路由 +
    新增**传输适配器注册表**（`cmd_transport_t {name,mask,start}`，
    `Cmd_TransportRegister`）与**流式会话助手**
    （`cmd_session_t` + `Cmd_SessionFeed` 按行切分分发）+ 统一提示符 `CMD_PROMPT`；
  - `cmd_catalog.h/c`（命令目录）：全部 cmd_* 实现从 shell.c 迁出，
    只声明 transport 掩码，与物理协议完全解耦；
  - `shell.c`（UART 适配器）：仅保留行编辑器/历史/补全/任务，
    初始化注册 UART 传输 + 命令目录；
  - `tcp_svc.c`（TCP 适配器）：改用 `Cmd_SessionFeed`（与未来 CAN 同一套
    会话逻辑），初始化注册 TCP 传输，提示符统一；
  - `cmd_can.h/c`（CAN 适配器模板）：`CMD_ENABLE_CAN=0` 惰性编译，
    文档化接入步骤——1 个适配器文件 + 1 行注册即可，命令零改动。
- **验证**：
  - Keil 0 Error/0 Warning；GCC 交叉编译 0 错误；APP ctest 通过；
  - HOSTLINK OTA build 234→235 通过，复位日志 `last build 235`、
    17 模块、ETH ready、TCP console listening、无活动态 CRASH；
- **双终端实测**：UART（COM9）与 TCP（:9000）均可用同一命令集
    （ver/echo/help/tcp/net），统一提示符 `D00> `；传输掩码生效
    （`ota` 标注 `[UART]`）；`net ip` 经 UART 修改网参后 TCP 终端
    立即可达（跨端一致性验证）。

### 12.82 配套上位机：D00Term 命令行终端（HOST/D00Term）
- **定位**：与固件 `cmd_transport_t` 对称的可插拔传输 CLI 终端——
  界面只有端口选择与命令行，无多余控件；UART 原始透传（设备自带
  行编辑/回显/补全）、ETH 行编辑会话（本地回显+上下键历史）、
  CAN 预留扩展类（`TRANSPORTS` 一行注册，命令零改动）。
- **形态**：单文件 `d00term.py` + `start_term.bat` 双击启动；
  PyInstaller 可选打包 `dist/D00Term/D00Term.exe`（1.8MB，已验证）。
- **能力**：`d00term.py com9 / tcp <ip> [/ tcp <ip> <port>]` 进入会话；
  `-x "cmd"` 单次执行（脚本化）；`--list` 枚举串口；`--selftest` 传输层自检。
- **验证**：`--selftest/--list` 通过；UART `ver`/`echo` 应答正确；
  ETH（192.168.10.10:9000）`ver`/`tcp` 应答正确（含 banner 与统一提示符）；
  板子 IP 测试后已恢复 192.168.1.10。
- **修复**：交互菜单选 UART 后原先直接取枚举第一个串口（可能非 COM9）——
  改为二次提示选择串口（默认优先 COM9），连接失败时提示可用串口清单。

### 12.83 构建全增量 + IP 持久化 + ETH 同网段默认（v192/build 237）
- **构建全增量（硬性要求）**：`Invoke-UV4` 默认 `-b`（只编改动文件）+ `-j0`
  并行，`-Clean` 才全量 `-r`；GCC 走 ninja 增量。实测 BOOT+APP 全量约 8 分钟
  → 增量约 21 秒（只改 1 个 .c 时）。
- **IP 持久化（`net ip`）**：
  - 新增 `BSP/bsp_nvm`（PARAM 扇区 11 flash 抽象：擦除/编程/读，互斥串行化）；
  - 新增 `SystemServices/net_config`（日志式 NVM：32B 槽 ×128，magic+seq+CRC，
    追加写免频繁擦除；满 128 次整扇区维护并保留 BOOT 参数双槽原样重写）；
  - `net ip <a.b.c.d>` 立即生效并保存 flash；上电自动应用上次配置；
    `net ip default` 清除并恢复出厂 192.168.1.10；
  - **CRC 长度 bug**：初版 `NET_CFG_CRC_LEN=22` 把 crc 字段自身算入导致扫描
    永远无效（复位后回默认），改为 20（magic..gw）后复位实测恢复成功。
- **OTA 会话残留修复**：BOOT 升级提交成功后失效全部 DOWNLOAD 会话槽
  （768×魔数写 0），杜绝"同版本+同尺寸"旧包续传混合（实测 err=3 根因）；
  新增协议 `CMD_OTA_RESET`（0x0D）+ CLI `--no-resume`（先复位会话再全新下载），
  auto_ota 默认走全新下载；版本升 v192。
- **D00Term ETH 同网段**：自动枚举电脑物理网卡（过滤 VMware/ICS/APIPA），
  依次探测"出厂 IP 可达→各网段 .10 可达→兜底首个网段"，无参 `tcp` 即连；
  配合 `net ip` 持久化：UART 设置一次，之后 TCP 每次上电即用。
- **验证**：Keil/GCC 0 警告；BOOT DAP 烧录通过；OTA v192/build 237 全新下载
  通过；`net ip 192.168.10.10` 保存→复位日志 `NVM cfg: saved IP` + `app ready
  (saved IP ...)`→TCP 可达；`net ip default`→复位恢复静态 192.168.1.10；
  D00Term 探测命中 192.168.10.10，无参 `tcp -x ver` 返回 v192。

### 12.84 EEPROM 存储架构（驱动 + KV + 触摸校准，build 238~241）
- **引脚排查（实测结论，纠正旧记录）**：
  - 工程日志 12.6 曾记录"I2C1(PB8/PB9) 接板载 24C02"——**实测证伪**：
    在 PB8/PB9 使能 I2C1 AF4 后整条 I2C1 总线故障（MPU6050 也掉线），
    回退后恢复 → PB8/PB9 为 **CAN1_RX/TX（带收发器，输出推挽对打）**；
  - 双总线扫描（0x40-0x77）：I2C1 仅 0x68（MPU6050）+ 0x77（疑似外接
    BMP280 类）；I2C2（PB10/PB11）为空；**0x50 两总线均无应答 →
    本板无可用板载 24C02**（或未贴片/损坏）。
- **存储架构（分层，已实现）**：
  - `BSP/bsp_i2c`：I2C1/I2C2 双总线互斥（HAL 非线程安全）；
  - `BSP/bsp_eeprom`：AT24C02 驱动（0x50，256B，8B 页写、总线自恢复），
    挂 **I2C2（PB10/PB11，空闲无冲突）**；
  - `SystemServices/kv_store`：EEPROM KV（头 16B + 10 槽 ×24B，
    key/len/crc16/data≤20B，任意字节就地改写）；
  - APP 集成：`touch cal`/`touch nudge` 完成即写 KV（key1），
    TouchSvc 上电恢复校准（工程日志 12.60 遗留项落地）；
    `kv <info|scan|get|set|erase|reset>` 命令（含双总线地址扫描）。
- **验证**：Keil/GCC 0 警告；OTA build 238→241 通过；MPU6050 总线正常
  （0x68 应答）；KV 命令优雅降级（EEPROM 缺席时 valid=0，触摸校准回落内存态）。
- **待办**：外接 AT24C02 模块至 PB10/PB11（I2C2，0x50）即可实机验证
  KV 读写/掉电保持/触摸校准恢复全链路；0x77 器件待确认型号。

### 12.85 更正：板载 24C02 = PB8/PB9 软件 IIC（官方驱动，build 242）
- **官方资料核实（正点原子探索者 STM32F407 开发指南）**：
  - 第 29 章 IIC 实验明确板载 24C02 的 SCL/SDA 分别连接 **PB8/PB9**，
    7 位器件地址 **0x50**（写 0xA0 / 读 0xA1）；
  - 官方 `myiic.h`：`IIC_SCL = PBout(8)` / `IIC_SDA = PBout(9)`，
    **软件模拟 IIC（GPIO 位操作 + 开漏/上拉）**驱动，非硬件 I2C 外设。
- **更正 12.84 的错误结论**：
  - 12.84 曾把"PB8/PB9 使能硬件 I2C1 AF4 后总线故障"误判为
    "PB8/PB9 是 CAN1、本板无板载 24C02"——**结论错误**；
  - 根因是**硬件 I2C1 外设与板载接线不兼容**，与引脚占用无关；
  - 正确做法即官方方案：软 IIC 位操作 PB8/PB9，与 MPU6050
    （硬件 I2C1/PB6-PB7）物理隔离、无总线互斥问题。
- **改造（上层零改动）**：
  - `BSP/bsp_eeprom` 重写为软件 IIC：PB8=SCL/PB9=SDA、开漏+上拉、
    DWT CYCCNT 微秒延时（约 250kHz 数据位 / 4us 起始停止）、
    ACK 超时、9 时钟总线自恢复；内部 FreeRTOS 互斥串行化；
    接口 `Init/Probe/Read/Write` 保持不变，KV/触摸上层零改动；
  - `kv scan` 改为：I2C1 硬件扫描（0x68/0x77）+ 软 IIC 探测 0x50。
- **验证**：Keil 增量 0 警告；OTA build 241→242 通过；
  `kv scan` 显示 `Soft IIC (PB8/PB9): 0x50 = OK (AT24C02)`；
  `kv set/get` 复位后保持；触摸校准存盘恢复链路确认。

### 12.86 ICMP 应答服务（icmp_svc，build 243）
- **定位**：板载 ICMP Echo 应答 + 完整可观测性，按服务分层嵌入：
  - `Application/icmp_svc`：raw ICMP PCB 接管 echo request（type 8），
    自组 echo reply（type 0，软件校验和），DWT 统计应答耗时（us）；
    与 eth_app 的 ping 客户端（`net ping`）天然共存（请求由本服务应答，
    应答仍由 ping 客户端匹配）；
  - 统计：rx echo / tx reply / drop / other、最近 1s 速率与峰值、
    min/avg/max 延迟、最近对端与 seq；
  - 限速（默认 500 pps，超限吞包）与静默模式（`reply off` 吞包不回）；
  - module.c 注册（优先级 66，EthApp 之后）；统一命令 `icmp
    <info|reset|reply on|off|limit pps>`（UART/TCP 均可用）；
    sysmon 新增 ICMP 监控项；CMake/Keil 工程同步加入。
- **验证（板侧全链路实测）**：Keil 增量 0 警告；OTA build 242→243；
  `icmp` 显示 reply ON/limit 500；PC ping → rx echo=reply、RTT≈26-83us、
  last peer=192.168.10.201；`reply off` 后 ping 3 包 → rx+3、reply+0、
  drop+3（静默生效）；`limit 1` 后连续 2 ping → reply+1、drop+1、
  peak=2（限速生效）；TCP/UDP（7777 回显、9000 控制台）不受影响。
- **PC 侧表象（曾被误判为环境问题，根因见 12.87）**：Windows ping 100%
  超时但板侧 rx=reply（回包帧逐字节校验正确：IP/ICMP 校验和、id/seq、
  源目 MAC 全对；同尺寸 UDP 回显正常到达 PC）。`netstat -s -p icmp`
  显示 PC 的 ICMP **Errors 随每个回包 +1**——即 Windows 收到但判为
  校验和错误后丢弃，与防火墙/WFP 驱动无关（12.87 定位为 F4 MAC 硬件
  校验和插入损坏 ICMP 校验和）。

### 12.87 根因与修复：F4 MAC TX 校验和插入损坏 ICMP（build 245）
- **根因（STM32F4 已知硅问题）**：`ethernetif.c` 的
  `TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC`，
  且 HAL 初始化 TX 描述符时 `SET_BIT(DESC0, ETH_DMATXDESC_CHECKSUMTCPUDPICMPFULL)`
  ——MAC 在发送时对 IP/TCP/UDP/**ICMP** 帧做硬件校验和插入，把软件
  已写好的 ICMP 校验和字段重写为错误值（F4 的 ICMP 校验和插入不可靠），
  于是 PC 侧 `netstat` ICMP Errors 递增、ping 全丢；而 IP/TCP/UDP 由
  MAC 重算后恰好正确，故 TCP/UDP 一切正常，极具迷惑性。
- **修复**：lwipopts.h 已全部开启软件校验和（CHECKSUM_GEN_IP/TCP/UDP/
  ICMP=1），只需在 `ethernetif.c` 把 `ChecksumCtrl = ETH_CHECKSUM_DISABLE`
  **并保留 CSUM attribute**（否则 HAL 每帧不写 CIC 位，残留
  CHECKSUMTCPUDPICMPFULL 仍生效）。此后所有校验和由软件计算，帧内容
  原样上线。
- **验证（build 245，双向全通）**：
  - PC `ping -n 5 192.168.10.10` → **5/5 成功，RTT 1-2ms**，ICMP Errors
    归零（回包全部计入 Echo Replies）；
  - 板 `net ping 192.168.10.201` → `Reply ... time=2ms`（PC 正常回包）；
  - TCP 控制台（:9000）、UDP 回显（:7777）不受影响；
  - `icmp` 统计：rx echo=reply、RTT 26-70us、限速/静默模式均正常；
  - 复位后 `net ip` 持久化恢复 192.168.10.10 正常。

### 12.88 存储架构整理：用户数据统一 EEPROM，OTA 参数保留 flash（build 248）
- **架构定界（明确分层）**：
  - **用户数据 → EEPROM（板载 AT24C02 256B）**：触摸校准、网络配置、
    以及未来所有需要掉电保持的用户参数，统一经 `usr_store` 存取；
  - **OTA 参数 → flash 不变**：BOOT 参数双槽（PARAM 0x080E0000）、
    APP 魔数/版本、DOWNLOAD 会话槽等全部保留原址原语义；
  - 删除 `bsp_nvm`（PARAM 区 flash 访问抽象不再被用户数据使用），
    PARAM 区从此只归 OTA/BOOT 所有，互不干扰。
- **usr_store（日志式 EEPROM 用户存储，替代 kv_store）**：
  - 布局：256B 顺序日志，记录 = magic(1)+key(1)+len(1)+crc16(2)+data；
    追加写天然免擦、分散磨损；删除 = 追加 len=0xFF 墓碑；
    空间不足自动 compact（收集各 key 最新值 → 整片写 0xFF → 重建）；
  - 读 = 单遍扫描取各 key 最新有效记录，CRC16 全程校验；
  - 键注册表集中管理（usr_store.h 加一行即登记新用户数据）；
  - 内部互斥串行化，命令 `kv` 升级为 `usr <info|scan|get|set|erase|reset>`。
- **net_config 迁移 EEPROM**：`NvConfig_*`（flash 日志）→
  `NetConfig_*`（usr_store key=NET_CFG），`net ip` 保存/上电恢复/
  `net ip default` 清除全链路走 EEPROM；日志文案同步改为 EEPROM。
- **验证（build 248，全链路实测）**：
  - 首启自动识别旧 KV 格式并重格式化；`[USR] ready (log ...)`；
  - `net ip 192.168.10.10` → `usr get 2` = C0A80A0AFFFFFF0000000000
    （ip/mask/gw）；**跨 OTA/复位均自动恢复 192.168.10.10**
    （修复了旧 flash 方案 OTA 后偶发回退默认 IP 的问题）；
  - `usr set 9 DEADBEEF` → get 一致 → 复位保持；`usr erase 9` →
    scan 见 tomb、get 返回 not found；`net ip default` → key2 墓碑、
    复位回 192.168.1.10；
  - PC ping 3/3（RTT 1-2ms）不受影响；Keil/GCC 0 警告；
    OTA build 245→248 连续三次成功（BOOT 参数/会话槽不受影响）。

### 12.89 ETH 全层服务打通 + 内存优化（build 273）
- **新增服务（Application 层，模块注册/命令/sysmon 全接入）**：
  - `dns_svc`：`dns <info|server <ip>|resolve <host>>`，服务器地址持久化 EEPROM；
  - `sntp_svc`：RFC4330 最小客户端，`sntp <info|sync [server]|auto on|off>`，
    同步写入 RTC（UTC+8），周期自动校时；
  - `mqtt_svc`：lwIP MQTT 客户端，`mqtt <connect|disconnect|pub|sub|info>`，
    连接成功后每 5s 自动发布设备遥测 JSON（d00/status）；
  - `http_svc`：最小 HTTP/1.0 状态服务（:8080），`GET /` HTML 页 +
    `GET /api/status` JSON；
  - `dhcp`：`dhcp <on|off|status>`，15s 无服务器自动回退保存的静态 IP。
- **内存优化（实测生效）**：
  - FreeRTOS 堆 48KB→52KB（CCM），启动任务栈 1KB→4KB（lwip_init 链深）；
  - 启用 LWIP_DNS/LWIP_MQTT（MEMP_NUM_SYS_TIMEOUT 5→8 编译校验）；
  - 板载资源：SRAM1 44KB/128KB、CCM 64KB（堆52KB+事件总线+LA）、ROM 218KB/320KB、
    运行时 free heap ≈15KB（全服务在线）。
- **排障记录（重要教训）**：
  - `.sram2` 段必须 `zero_init`（与 `.ccmram` 一致）：UNINIT 区若携带加载镜像，
    __main 启动加载崩溃；加 zero_init 后 SRAM2 布局可正常启动（build 262 验证）；
  - **DNS×SRAM2 组合会导致启动 HardFault（IMPRECISERR，ETH_DMATxDescListInit，
    startupTask 18ms）**：LWIP_DNS 开启且 ETH 描述符/RX 池置于 SRAM2 时崩溃；
    DNS 关闭或描述符回 .bss 均正常。机理待进一步定位（疑似 DNS 使 lwip 初始化
    与 SRAM2 描述符写产生竞态/总线异常），**当前交付配置：描述符/RX 池留在
    SRAM1 .bss，SRAM2 暂不启用**；堆优化与服务不受影响；
  - 调试用 KEIL DAP（UV4 `-d` + 调试 ini 可刷写/读现场）；
    STM32CubeProgrammer 识别不到该 CMSIS-DAP。
- **验证（build 273，全链路实测）**：
  - `http://192.168.10.10:8080/` HTML 200 + `/api/status` JSON；
  - `dns resolve dev.local` → 192.168.10.10（PC 临时 DNS 服务器 :53）；
  - `sntp sync` 逻辑与换算算法验证正确（PC :123 被 w32time 占用，需真实
    SNTP 服务器或管理员权限停用 w32time 后复测）；
  - `dhcp on` → 15s 超时 → 自动回退静态 192.168.10.10（实测）；
  - `mqtt connect` 无 broker → 状态机 CONNECTING→DISCONNECTED→IDLE（计数正确），
    待真实 broker 复测 pub/sub/遥测；
  - ICMP ping 3/3（1-2ms）、TCP 控制台、UDP 回显均正常；Keil/GCC 0 警告。

### 12.90 LCD 全套 ETH 状态页 + ETH 全套压力性能测试（build 278）
- **LCD NET 页升级（优雅美观）**：
  - 四分组布局：LINK（Link/IP/MAC/GW/DHCP）、TRAFFIC（RX/TX/Uptime）、
    ICMP（Echo rx/tx/drop、Rate、RTT min/avg/max、Peer）、
    SERVICES（TCP cli/acc、MQTT 状态、HTTP 请求数、DNS 服务器、SNTP 时间）；
  - 排版规范：FONT12 右对齐（7px/字符）、标签左列 x=8、值右缘 x=234，
    分组标题黄色 + 分隔线，状态值语义着色（UP/bound/ONLINE=绿，
    DOWN/ERR=红），刷新先清值区后画杜绝残影；`lcd page <0-5>` 直达各页。
- **压力性能测试（build 278 实机）**：
  - ICMP：常规 50×32B 与 20×1400B 全通（RTT 1-2ms）；并发 100 全收全回
    （peak 4pps，板侧 RTT 26-232us）；限速 limit=5 下 250 洪峰 → 回 132/丢 118
    （限速精确生效，系统稳定）；
  - UDP 回显：持续 181 pkt/s 零丢失（500/500，avg 5.5ms）；突发 8 连发
    14.8% 丢失（RX 零拷贝池深 8 的设计边界，已记录）；
  - TCP 控制台：670 cmd/s（avg 1.5ms，100/100）；并发 2 客户端成功；
  - HTTP /api/status：200/200 全通，avg 65ms / max 84ms / 15.3 req/s；
  - DNS resolve：30/30 稳定；
  - 稳定性：历经全部压力后堆 15216→15128B（无泄漏），全部任务栈水位健康
    （HttpSvc 265、tcpip 366、EthIf 443、LcdUI 542 / 各自限额）；
- **压测暴露并修复的 bug（http_svc）**：
  - `netconn_recv_tcp_pbuf(conn, NULL)` 吞掉请求导致后续 recv 阻塞；
  - `http_handle` 的 2×1024B 局部缓冲压爆 2048B 服务任务栈（HTTP 间歇
    卡死根因）→ 改静态缓冲；修复后 200/200 全通。
