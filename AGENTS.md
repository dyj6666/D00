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
| RUN(APP) 起始 | 0x08010000，832KB，扇区 4-10（方案B：含原 BACKUP+DOWNLOAD） |
| 回滚源 | 外部 Flash img_lib（0x200000，升级前备份当前 RUN，PENDING 回滚） |
| 下载暂存 | 外部 Flash ota_dl（0x000000，2MB，单槽 1MB） |
| PARAM | 0x080E0000，128KB |
| APP 有效魔数 | 0x4F54412E @ 0x080DFFF8 |
| APP 版本号 | 0x080DFFFC |
| 升级标志 | RTC_BKP_DR1 == 0x5A5A |
| APP 调试/Shell 控制台 | USART3（当前物理 CH340，COM 号动态；@115200） |
| APP HOSTLINK 上位机链路 | USART1 |
| BOOT 日志 | USART2 |


## 3. 工具链（固定路径，见 workflow/common.ps1）

- Keil：`D:\MDK\CORE\UV4\UV4.exe`（发布构建；构建前先关闭 Keil IDE 避免冲突）
- 烧录：`D:\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe`
- Python：`D:\Python\python.exe`
- GCC 交叉编译：cmake + Ninja + arm-none-eabi-gcc

> **烧录约定（2026-08-13 起）**：APP 固件更新**一律走 OTA**
> （TCP :9020 / HTTP / UART / CAN），不再用 DAP 烧 APP——CMSIS-DAP 探针
> 连续烧录 320KB 约 20s 即 HID 超时假死（多次中断烧录需物理重插）。
> DAP 仅用于 BOOT 区更新。

## 4. 自动开发闭环（全自动模式：每轮任务默认执行）

完成任何固件改动后，按影响范围自动执行，直到全部通过或明确报告失败原因：

1. 环境自检：`workflow\self_check.ps1`（工具链/串口/硬件在线 + 源码编码/崩溃后门/
   版本单一事实源/可复现性/文档漂移一致性检查）
2. 构建：`workflow\auto_build.ps1 -Project ALL -Toolchain Keil`（BOOT + APP 都必须过；
   **默认增量** Keil `-b` / GCC ninja，确需全量时加 `-Clean`）
3. 若硬件在线：`workflow\auto_flash.ps1`（SWD 烧 BOOT + 带魔数 APP 镜像，逐扇区擦写并校验）
4. 日志验证：`workflow\auto_verify.ps1`（复位后抓 COM9，检查启动完成/ETH ready/OTA Agent ready，
   且无活动态 CRASH/硬错误；历史崩溃恢复记录只提醒、不计失败）
5. 若改动涉及 OTA/BOOT/下载链路：`workflow\auto_ota.ps1`（HOSTLINK 安全升级冒烟，全程抓 BOOT/APP 日志）
6. 主机单测：`workflow\auto_hosttest.ps1`（BOOT host_tests + APP 协议测试；
   `-Robustness` 可加跑 LogicAnalyzer 硬件健壮性，需物理接线）
7. 一键总流水线：`workflow\auto_pipeline.ps1 -Mode full [-IncludeOta]`（产出 `workflow\last_report.json`）
8. 同步更新对应 `ENGINEERING_LOG.md`；**重点问题必须按
   `docs\ISSUE_POSTMORTEM_TEMPLATE.md` 完整记录排查过程（见第 8 节）**
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

## 7. 注释规范

- 全仓统一遵循 `docs\COMMENT_STYLE.md`：C 文件头横幅 / 区段虚线 / Doxygen 函数块 /
  关键语句行尾注释；Python 模块与函数 docstring；PS1 脚本头横幅。
- 关键语句必须有"看得懂"的注释（做什么/为什么），禁止复读式注释。

## 8. 重点问题复盘硬性规定

- 满足以下任一条件的问题，**必须**按 `docs\ISSUE_POSTMORTEM_TEMPLATE.md`
  完整记录到对应 `ENGINEERING_LOG.md`：
  1. 需要超过 3 步排查才定位；
  2. 排查过程中出现过被推翻的错误方向；
  3. 修复涉及协议/时序/并发/底层配置等系统性根因；
  4. 最终采用工程权衡（如"稳健优先于理论极限"）而非彻底根除。
- 记录内容**必须包含完整的排查过程**：现象证据 → 排查思路 → 每步实验与
  观察（含被推翻的方向与原因）→ 根因机理 → 解决方案 → 验证数据 → 经验沉淀。
  **禁止只写结论**；分析过程比结论更重要。
- 提交前自检：本次任务涉及的重点问题是否已按模板回填日志；未回填视为未完成。
