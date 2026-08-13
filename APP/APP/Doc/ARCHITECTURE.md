# 软件架构与开发指南

## 1. 分层结构

```
┌─────────────────────────────────────────────────────────────┐
│ Application          业务模块（按键/灯/OTA/数据上报）        │
│   key_app  led_app  ota_agent  data_agent                  │
├─────────────────────────────────────────────────────────────┤
│ SystemServices       系统服务（平台无关）                    │
│   event_bus  消息总线（静态池）                             │
│   var_manager 变量管理器 / var_list 分片                    │
│   data_link   上位机协议 / protocol 纯逻辑帧层              │
│   logger/shell/sysmon/watchdog 调试与可靠性                 │
│   la_sample/la_buffer/la_trigger 逻辑分析仪（平台模块）      │
├─────────────────────────────────────────────────────────────┤
│ BSP                 板级支持包（唯一允许碰硬件的地方）        │
│   bsp_system 复位/延时/复位原因                             │
│   bsp_uart   DMA 串口（调试口 + 上位机口）                  │
│   bsp_watchdog / bsp_rtc / bsp_gpio                         │
├─────────────────────────────────────────────────────────────┤
│ Config + Core       配置集中 + 平台初始化（CubeMX 生成）     │
│   app_config/pinout/var_ids + Core/*（HAL、FreeRTOS）        │
└─────────────────────────────────────────────────────────────┘
```

**依赖规则**：上层只依赖下层接口。Application/SystemServices 禁止直接调用 HAL
（`stm32f4xx_hal.h` 等）；**已登记的平台模块豁免**（与芯片强相关/故障上下文）：

- `la_*`（逻辑分析仪 DMA 采样引擎）
- `err_mgr`（故障处理需裸寄存器/关中断上下文，无法走 HAL）
- `signal_gen`（产线信号发生器，软件位操作测试工具）

其余越层由 `Script/check_layering.py` 在 CI 守门（禁用 HAL 头与向上 include）；
`main.h`（CubeMX 总头，内含 HAL）命中记 WARNING，应逐步收敛到 BSP 接口。

**组合根例外**：`SystemServices/module.c`（模块注册表）与命令目录
`Application/cmd_catalog.c` 需要"认识"全部模块以完成接线——这是声明式
组合根模式，属有意为之；其余 SystemServices 禁止向上依赖 Application。
`cmd_shell`（通用分发器）与 `cmd_can`（传输适配器）保持平台无关。

## 2. 如何新增一个模块

### 2.1 新增系统服务 / 应用模块

1. 在 `Application/` 或 `SystemServices/` 新建 `xxx.c/h`，实现 `Xxx_Init(void)`。
2. 在 `SystemServices/module.c` 注册表添加一行：
   ```c
   MODULE_INIT("Xxx", <priority>, Xxx_Init),
   ```
   priority 越小越先初始化（依赖其他模块请给更大值）。
3. 把 `.c` 加入构建：
   - Keil：`MDK-ARM/APP.uvprojx` 对应组（或用 Script 辅助）。
   - GCC：`CMakeLists.txt` 的 `FW_SOURCES`。
4. 需要跨模块通信：在 `msg_types.h` 注册消息类型，然后
   `EventBus_Subscribe(type, handler)` 订阅、`MSG_SEND_SIMPLE/DATA` 发布。
5. 需要出现在 `sysmon` 报告里：在模块 `Init` 调用
   `SysMon_RegisterItem("名称", print_fn)`，避免 sysmon 向上依赖应用模块。

### 2.2 新增外设驱动

1. 平台初始化（引脚/时钟）放 Core 的 CubeMX 生成文件；
2. **驱动功能接口必须放进 `BSP/`**（如 `bsp_xxx.c/h`），服务层只调用 BSP 接口；
3. 硬件映射（引脚号）写进 `Config/pinout.h`，软件参数写进 `Config/app_config.h`。

### 2.3 暴露变量给上位机

1. 在 `Config/var_ids.h` 登记唯一 ID（禁止在模块里写数字）；
2. 模块初始化时 `VAR_Register(VAR_ID_XXX, "name", type, perm, &var)`；
3. 上位机 `LIST_VARS` 自动分片下发，订阅后 DataAgent 周期上报。

### 2.4 新增 shell 命令

在 `Application/cmd_catalog.c` 添加 `cmd_xxx` 实现 + `cmd_table` 一行，
声明允许的传输掩码（`CMD_TRANSPORT_UART/TCP/...`）。命令实现与具体物理传输
（串口 / TCP / 未来 CAN）解耦：`LOG_Printf` 输出自动路由到当前终端。

> 命令目录位于 Application 层（应用级接线），由模块注册表（CmdCat，prio 3）
> 在 Shell 启动前注册；`cmd_shell` 是平台无关的通用分发器。

### 2.5 Shell 传输适配器（新增物理协议）

命令核心 `cmd_shell` 维护传输适配器注册表（`cmd_transport_t`）：
UART（`shell.c`）、TCP（`tcp_svc.c`）已注册；新增协议（如 CAN）只需
实现一个适配器文件 + `Cmd_TransportRegister()` 一行，命令目录零改动。
流式传输（TCP/CAN）复用 `Cmd_SessionFeed` 按行切分分发。

## 3. 移植指南

### 3.1 同系列 STM32（推荐路径）

1. CubeMX 按新芯片重新生成 Core（保留 USER CODE 区域）；
2. 修改 `Config/pinout.h`（引脚）、`Config/app_config.h`（参数）；
3. 核对 BSP 实现中的外设实例与 DMA 映射；
4. 调整链接脚本 `Core/Startup/*.ld` 的 FLASH 起点/容量。

### 3.2 跨厂商平台

1. **只重写 `BSP/` 目录**：`bsp_system`（复位/延时）、`bsp_uart`
   （DMA+IDLE 收发）、`bsp_watchdog`、`bsp_rtc`、`bsp_gpio`；
2. `la_*` 平台模块按新平台实现（或裁剪）；
3. 其余（event_bus、var_manager、protocol、var_list、crc16、module、watchdog
   逻辑、host_tests）均为平台无关代码，直接复用；
4. `protocol.c`/`var_list.c`/`crc16.c` 已有主机单元测试，可作为移植回归基线。

## 4. 构建与测试

### Keil（主要发布路径）

```
UV4 -r -b MDK-ARM/APP.uvprojx -j0 -o build.log
```

### GCC（交叉编译验证路径）

```
cmake -S . -B build-fw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build-fw
# 产物：build-fw/APP.elf、build-fw/APP.bin
```

### 主机单元测试（纯逻辑层，无需交叉工具链）

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`CMakeLists.txt` 由 `Script/gen_cmake.py` 从 Keil 工程生成：
交叉编译（带 toolchain file）时构建固件，纯主机构建时自动进入 `host_tests` 分支
（协议 / CRC / 变量分片）。

### 语法冒烟（无 Keil 环境时）

```
powershell -File Script/check_firmware_syntax.ps1
```

## 5. 可靠性机制

- 硬件看门狗（IWDG）：SysMon 软件定时器喂狗；
- 任务级看门狗（watchdog）：EventBus/DataAgent 心跳，超时软件复位；
- 上位机协议：CRC16 校验 + 帧长校验 + 错误响应 + 坏帧重同步；
- 事件总线：静态消息池，溢出计数可在 `sysmon` 查看；
- 变量访问：互斥锁 + 超时保护。

## 6. 工程日志

历次问题的完整复盘（现象/根因/解决/验证）见 [工程日志](ENGINEERING_LOG.md)。
