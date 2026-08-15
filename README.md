# D00 — STM32F407 综合开发平台

单仓库三子工程，覆盖**引导（BOOT）/ 应用（APP）/ 上位机（HOST）**全链路。
配套 **AI 工作流**（`workflow/` + `AGENTS.md`）：改代码 → 构建 → 烧录 → 日志验证 → OTA 冒烟 → 单测 → 提交 全自动闭环。

## 仓库结构

```
D00/
├── APP/     应用固件工程（FreeRTOS + HOSTLINK 协议 + 逻辑分析仪）
├── BOOT/    安全 OTA 引导工程（AES/ECC/SHA256 验签 + YMODEM 传输）
├── HOST/    上位机（VLink_Debugger 调试器 + OTA_Tool 升级工具 + LogicAnalyzer 逻辑分析仪 + D00Term 命令行终端）
├── workflow/ Codex AI 工作流（自检/构建/烧录/验证/OTA/单测/报告）
├── config/   机器可读配置（版本号单一事实源 version.json）
├── .github/  云端 CI（BOOT+APP GCC 构建、ctest、HOST 测试）
├── .githooks/ git hooks（pre-commit 编码/卫生检查）
├── AGENTS.md Codex 行为准则（最高执行约定）
├── install.ps1 一键启用 git hooks
├── .gitignore   统一忽略规则（Keil/GCC/Python 产物）
└── LICENSE
```

## 三个子工程

| 工程 | 定位 | 关键能力 |
| --- | --- | --- |
| **APP** | 业务固件 | 事件总线、变量管理器、HOSTLINK 协议、任务级看门狗、8 通道逻辑分析仪（条件触发 / DMA 双缓冲） |
| **BOOT** | 安全引导 | 升级标志检测、APP 有效性魔数校验、YMODEM 接收、AES-CTR 解密、ECC/SHA256 验签、防回滚、跳转前外设清理 |
| **HOST** | 上位机 | VLink_Debugger（HOSTLINK 客户端/波形）、OTA_Tool（安全固件升级）、LogicAnalyzer（8 通道逻辑分析仪） |

## 构建指引

### APP（固件）
```bash
# Keil（发布路径）
UV4 -r -b APP/APP/MDK-ARM/APP.uvprojx -j0

# GCC 交叉编译（验证路径）
cmake -S APP/APP -B APP/APP/build-fw -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=APP/APP/cmake/arm-none-eabi-toolchain.cmake
cmake --build APP/APP/build-fw

# 主机单元测试（协议层；无需交叉工具链）
cmake -S APP/APP -B APP/APP/build && cmake --build APP/APP/build
ctest --test-dir APP/APP/build
```

### BOOT（固件）
```bash
# Keil
UV4 -r -b BOOT/BOOT/MDK-ARM/BOOT.uvprojx -j0

# GCC 交叉编译
cmake -S BOOT/BOOT -B BOOT/BOOT/build-fw -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=BOOT/BOOT/cmake/toolchain-stm32f4.cmake
cmake --build BOOT/BOOT/build-fw

# 主机单元测试（crc32 / fifo 纯逻辑）
cmake -S BOOT/BOOT -B BOOT/BOOT/build && cmake --build BOOT/BOOT/build
ctest --test-dir BOOT/BOOT/build
```

### HOST（上位机）
```bash
# VLink_Debugger：HOSTLINK 上位机
cd HOST/VLink_Debugger
pip install -r requirements.txt
python main.py
python tests/test_protocol.py   # 协议层单元测试

# OTA_Tool：BOOT 安全升级工具（加密打包 + YMODEM 发送，COM 口 115200）
cd HOST/OTA_Tool
python main.py                   # GUI 或参考 test_send.py 脚本化驱动

# LogicAnalyzer：8 通道逻辑分析仪（控制口 115200 + 数据口 921600）
cd HOST/LogicAnalyzer
pip install -r requirements.txt
python main.py
python tests/test_decoders.py    # 解码器单元测试

# D00Term：配套命令行终端（UART/ETH，CAN 扩展）
cd HOST/D00Term
python d00term.py com9            # 或 start_term.bat 双击启动；tcp 192.168.1.10 走 ETH
```

## 三工程联动契约

```
BOOT(0x08000000, 64KB) ──跳转──▶ APP(0x08010000, 832KB) ──HOSTLINK(921600)──▶ HOST
        │                        │
        └──OTA 升级：APP 写 RTC_BKP_DR1=0x5A5A → 复位 → BOOT 进升级模式
            → YMODEM 收包 → 验签解密 → 写 APP 区 → 写魔数/版本 → 复位跳转
```

关键约定（详见各工程日志，勿轻易改动）：

| 约定 | 值 |
| --- | --- |
| APP 有效性魔数 | `0x4F54412E` @ `0x080DFFF8` |
| APP 版本号 | @ `0x080DFFFC` |
| 升级请求标志 | `RTC_BKP_DR1 == 0x5A5A` |
| OTA 包头魔数 | `0x4F5441FE` |
| 跳转目标 | `0x08010000` |

> 分区/魔数/版本地址的唯一权威来源：`BOOT/BOOT/Config/boot_config.h` 与
> `APP/APP/Config/app_config.h`（另见 `BOOT/BOOT/Other/flash分区` 与 `APP/APP/Doc/OTA_ARCHITECTURE.md`）。

## AI 工作流

- 行为准则：[`AGENTS.md`](AGENTS.md)（Codex 最高执行约定）；
- 一键环境准备：`powershell -ExecutionPolicy Bypass -File install.ps1`（启用 pre-commit hooks）；
- 总流水线：`workflow\auto_pipeline.ps1 -Mode full -IncludeOta`，产出 `workflow\last_report.json`；
- 版本/构建号单一事实源：`config/version.json`（`workflow\common.ps1` 自动读取）；
- 云端 CI：`.github\workflows\ci.yml`（无硬件回归：BOOT+APP GCC 构建、ctest、HOST 测试、崩溃后门扫描）；
- 详细说明见 [`workflow/WORKFLOW.md`](workflow/WORKFLOW.md)。

## 文档索引

| 文档 | 位置 | 内容 |
| --- | --- | --- |
| APP 架构指南 | `APP/APP/Doc/ARCHITECTURE.md` | 分层规则、加模块/驱动/变量指南、移植路线 |
| APP 工程日志 | `APP/APP/Doc/ENGINEERING_LOG.md` | 全部问题复盘（现象/根因/解决/验证） |
| BOOT 工程日志 | `BOOT/BOOT/Other/ENGINEERING_LOG.md` | BOOT 问题复盘 + 联动契约 |
| HOST 说明 | `HOST/VLink_Debugger/README.md` | 结构、运行、测试 |
| HOST 升级工具 | `HOST/OTA_Tool/` | BOOT 安全 OTA 打包与发送（UID 派生密钥 + ECC 签名） |
| HOST 逻辑分析仪 | `HOST/LogicAnalyzer/README.md` | 8 通道采集/波形/UART·I2C·SPI 解码 |
| D00Term 终端 | `HOST/D00Term/README.md` | UART/ETH 命令行终端（CAN 扩展） |
| AI 工作流 | `workflow/WORKFLOW.md` | 流水线模式、脚本参数、验证规则 |

## 协作约定

- **分层**：业务/服务/BSP/配置严格分层，服务层不直接碰 HAL；
- **构建产物**：一律不进 git（统一 `.gitignore`）；
- **编码**：UTF-8、4 空格缩进、Allman 括号（pre-commit 自动检查）；
- **单一事实源**：分区/魔数见 Config 头文件，版本/构建号见 `config/version.json`，禁止散落重复；
- **留痕**：任何修改同步更新对应工程的 `ENGINEERING_LOG.md`。
