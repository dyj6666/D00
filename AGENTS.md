# D00 AGENTS —— Codex 全自动嵌入式开发约定

本文件是 Codex（桌面版，可接入 DeepSeek V4 Flash 等模型）在本仓库内工作时的最高行为准则。
目标：让"改代码 → 构建 → 烧录 → 日志验证 → OTA 冒烟 → 单测 → 提交"全链路自动闭环，硬件在线时做到端到端验证。

## 1. 仓库地图

| 路径 | 内容 |
| --- | --- |
| APP/APP | 应用固件：FreeRTOS + HOSTLINK 协议 + 逻辑分析仪 + OTA 代理 |
| BOOT/BOOT | 安全引导：AES/ECC/SHA256 + YMODEM + 双区回滚 |
| HOST/ | 上位机：VLink_Debugger / OTA_Tool / LogicAnalyzer / EthLab |
| workflow/ | 全自动流水线脚本（本工作流） |
| config/ | 机器可读配置（版本号单一事实源 version.json） |
| .github/ | 云端 CI（无硬件回归：GCC 构建 / ctest / HOST 测试 / 后门扫描） |

## 2. 关键硬件约定（改动前先读，勿擅改）

| 项 | 值 |
| --- | --- |
| BOOT 起始 | 0x08000000，64KB，扇区 0-3 |
| RUN(APP) 起始 | 0x08010000，320KB，扇区 4-6 |
| BACKUP | 0x08060000，256KB |
| DOWNLOAD | 0x080A0000，256KB |
| PARAM | 0x080E0000，128KB |
| APP 有效魔数 | 0x4F54412E @ 0x0805FFF8 |
| APP 版本号 | 0x0805FFFC |
| 升级标志 | RTC_BKP_DR1 == 0x5A5A |
| 调试串口 | COM9 @ 115200 |
| HOSTLINK | COM13 @ 921600 |

> 注意：仓库根 README 中"APP 魔数 @0x0804FFF8"已过时，以
> `BOOT/BOOT/Config/boot_config.h` 与 `APP/APP/Config/app_config.h` 为准（0x0805FFF8 / 0x0805FFFC）。

## 3. 工具链（固定路径，见 workflow/common.ps1）

- Keil：`D:\MDK\CORE\UV4\UV4.exe`（发布构建；构建前先关闭 Keil IDE 避免冲突）
- 烧录：`D:\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe`
- Python：`D:\Python\python.exe`
- GCC 交叉编译：cmake + Ninja + arm-none-eabi-gcc

## 4. 自动开发闭环（全自动模式：每轮任务默认执行）

完成任何固件改动后，按影响范围自动执行，直到全部通过或明确报告失败原因：

1. 环境自检：`workflow\self_check.ps1`（工具链/串口/硬件在线 + 源码编码/崩溃后门/
   版本单一事实源/可复现性/文档漂移一致性检查）
2. 构建：`workflow\auto_build.ps1 -Project ALL -Toolchain Keil`（BOOT + APP 都必须过）
3. 若硬件在线：`workflow\auto_flash.ps1`（SWD 烧 BOOT + 带魔数 APP 镜像，逐扇区擦写并校验）
4. 日志验证：`workflow\auto_verify.ps1`（复位后抓 COM9，检查启动完成/ETH ready/OTA Agent ready，
   且无活动态 CRASH/硬错误；历史崩溃恢复记录只提醒、不计失败）
5. 若改动涉及 OTA/BOOT/下载链路：`workflow\auto_ota.ps1`（HOSTLINK 安全升级冒烟，全程抓 BOOT/APP 日志）
6. 主机单测：`workflow\auto_hosttest.ps1`（BOOT host_tests + APP 协议测试；
   `-Robustness` 可加跑 LogicAnalyzer 硬件健壮性，需物理接线）
7. 一键总流水线：`workflow\auto_pipeline.ps1 -Mode full [-IncludeOta]`（产出 `workflow\last_report.json`）
8. 同步更新对应 `ENGINEERING_LOG.md`（现象/根因/解决/验证）
9. Conventional Commits 提交（如 `fix(boot): ...`）；构建产物与日志一律不进 git；
   提交前 pre-commit 钩子自动检查暂存区编码/行尾卫生

## 5. 报告与证据

- 每阶段结束后读取 `workflow\last_report.json`，把阶段结果、日志尾部、失败原因写进给用户的回复。
- 硬件验证必须以日志证据为准，禁止仅凭"编译通过"宣称完成。
- 版本号/构建号的**单一事实源**是 `config\version.json`（`workflow\common.ps1` 自动读取，
  `self_check.ps1` 校验一致性）；发版前必须同步固件内版本常量与 `OTA_Tool/version_lib.json` 登记。

## 6. 红线

- 不提交构建产物、`_auto_*.txt` 日志、`workflow\logs`、`workflow\last_report.json`。
- 不修改分区表/魔数/地址/升级标志约定，除非任务明确要求并同步更新文档。
- 烧录前确认目标串口无占用（COM9/COM13）；不擦 option bytes；不整片擦除 PARAM 区。
- 不把串口日志中的私有信息写进提交。
- `HOST/OTA_Tool/config.json` 与 `version_lib.json` 为本地状态（含设备 UID/本地路径），一律不入库。
- 保持 UTF-8、空格缩进、Allman 括号；新增文件遵循现有分层（业务/服务/BSP/配置）。
