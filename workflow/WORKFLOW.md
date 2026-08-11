# D00 全自动嵌入式开发工作流（AI Workflow）

基于 ChatGPT 桌面版 + Codex（可接入 DeepSeek V4 Flash 等模型）+ 现有 STM32 工具链的
"改代码 → 构建 → 烧录 → 日志验证 → OTA 冒烟 → 单测 → 提交" 全自动闭环。

## 一、架构总览

```
用户目标 ──> Codex（读 AGENTS.md 行为准则）
                │
                ├─1 self_check.ps1        环境/串口/硬件自检
                ├─2 auto_build.ps1        Keil 发布构建（BOOT+APP）或 GCC 快速构建
                ├─3 auto_flash.ps1        SWD 逐扇区擦写 BOOT+带魔数 APP 镜像，校验+复位
                ├─4 auto_verify.ps1       复位后抓调试口日志（默认 COM5，自动探测），关键字判定 PASS/FAIL
                ├─5 auto_ota.ps1          HOSTLINK 安全升级冒烟（数据口默认 COM13 921600）
                ├─6 auto_hosttest.ps1     主机单测（BOOT ctest + APP 协议测试）
                └─7 auto_pipeline.ps1     一键总流水线，产出 last_report.json 证据报告
                     │
                     └─> ENGINEERING_LOG.md 更新 + Conventional Commit
```

## 二、一键安装

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1
```

安装内容：启用 `.githooks\pre-commit`（暂存区 UTF-8/行尾卫生检查）。
`AGENTS.md`、`workflow\`、`config\version.json`、`.github\workflows\ci.yml` 均已入库，克隆后开箱即用。

## 三、怎么用

### 模式 A：对话驱动全自动（推荐日常使用）

1. 打开 ChatGPT 桌面版，左上角切换到 Codex；
2. 打开本地文件夹 `D:\GIT-SPACE\D00`；
3. 直接用中文描述任务，例如：
   - "修复 UART 偶发丢字节，改完自动构建、烧录、验证日志，通过后提交"
   - "给 OTA 加一个防回滚检查，跑完整流水线含 OTA 冒烟"
   - "帮我 review 最近的改动并跑一次 quick 流水线"
4. Codex 会依据 AGENTS.md 自动执行构建 → 烧录 → 日志验证 → OTA → 单测 → 提交闭环，
   每阶段读取 `workflow\last_report.json` 向你汇报证据。

### 模式 B：一键流水线（手动/CI 场景）

```powershell
# 全自动完整闭环（含硬件烧录+验证，可再加 OTA 冒烟）
powershell -ExecutionPolicy Bypass -File workflow\auto_pipeline.ps1 -Mode full -IncludeOta

# 常用子模式
-Mode quick      # 构建 + 主机单测（无需硬件）
-Mode build      # 仅构建
-Mode flash      # 烧录 + 日志验证（需要硬件）
-Mode verify     # 仅日志验证
-Mode ota        # 仅 OTA 冒烟（需要硬件+APP.bin）
-Mode hosttest   # 仅主机单测
-Mode selfcheck  # 仅环境自检

# 跳过开关：-SkipBuild -SkipFlash -SkipVerify -SkipOta -SkipHostTest -SkipSelfCheck
# OTA 版本参数：-Version 191 -BuildNo 232
```

### 模式 C：定时/监控自动化（无人值守）

在 ChatGPT 桌面版 Codex 中为该仓库线程创建自动化任务，例如：
- **每日回归**：每天固定时间跑 `auto_pipeline.ps1 -Mode quick`，把 `last_report.json` 摘要汇报到线程；
- **硬件在线监控**：定时跑 `self_check.ps1 -TestHw`，发现设备掉线或构建失败立即提醒；
- **发版任务**：发布前自动执行 `-Mode full -IncludeOta` 并要求提交。

### 模式 D：CI/团队扩展

服务器可用 arm-none-eabi-gcc 跑 `auto_build.ps1 -Toolchain GCC` 与 `auto_hosttest.ps1`；
烧录/OTA 等硬件步骤保留在本地工作站执行。

### 模式 E：云端 CI（无硬件回归）

`.github\workflows\ci.yml` 在每次 push/PR 自动运行：BOOT+APP GCC 构建、BOOT/APP ctest、
HOST 测试、发布固件崩溃后门扫描、工作流文件版本化检查。每次提交前 pre-commit 检查编码/行尾卫生。

## 四、脚本参考

| 脚本 | 作用 | 关键参数 | 产出 |
| --- | --- | --- | --- |
| self_check.ps1 | 工具链/串口/约定一致性 + 编码/崩溃后门/版本单一事实源/可复现性/文档漂移检查 | `-TestHw` 探测 SWD | 退出码 0/1/2 |
| auto_build.ps1 | Keil/GCC 构建 BOOT+APP（**默认增量**：Keil `-b` / GCC ninja；`-Clean` 才全量） | `-Project APP/BOOT/ALL` `-Toolchain Keil/GCC` `-Clean` | `workflow\logs\build_*.log` |
| auto_flash.ps1 | 生成带魔数镜像并 SWD 烧录 | `-SkipBoot -SkipApp -KeepImage -Version N` | `workflow\logs\flash.log` |
| auto_verify.ps1 | 复位抓调试口日志并判定 | `-Seconds 25 -NoReset -SerialReset`（DAP-only 环境用串口复位） | `APP\_auto_boot.txt` |
| auto_ota.ps1 | HOSTLINK 安全升级冒烟 | `-Version N -BuildNo N -Port COM13` | `workflow\logs\ota_hostlink.log` |
| auto_hosttest.ps1 | 主机单测（+ `-Robustness` LA 硬件健壮性，需接线） | `-SkipBoot -SkipApp` | `workflow\logs\hosttest_*.log` |
| auto_pipeline.ps1 | 总流水线编排 | `-Mode ...` `-IncludeOta` | `workflow\last_report.json` |

## 五、证据与报告

每次运行后读取 `workflow\last_report.json`，包含：

```json
{
  "tool": "d00-auto",
  "git": "a1b2c3d",
  "stages": {
    "selfcheck": "OK",
    "build": "OK",
    "flash": "OK",
    "verify": "OK",
    "hosttest_boot": "OK",
    "summary": "PASS"
  }
}
```

验证判定规则（`common.ps1` 中可调）：
- 期望关键字：`Modules initialized`、`ETH  : app ready`、`OTA  : Agent ready`
- 失败关键字：`HardFault`、`UsageFault`、`assert`、`FATAL`、活动态 `[CRASH]`（非 recovered 行）
- 提示信息：`[CRASH] Previous crash recovered` 属历史崩溃恢复记录，只提醒、不计失败

## 六、版本与构建号同步

版本/构建号的**单一事实源**是仓库根 `config\version.json`
（`workflow\common.ps1` 启动时自动读取，缺省回退内置值）。
`OTA_Tool` 的 `version_lib.json` 仅作本地登记（已 gitignore，不入库）。发版前必须：

1. 更新 `config\version.json`（写入 `0x0805FFFC` 的数值）；
2. 同步固件版本常量与 `OTA_Tool/version_lib.json` 登记；
3. 用 `-Version -BuildNo` 显式覆盖也可以。

固件内 `Ota_Begin` 会拒绝"版本号低于当前固件"的升级。
`self_check.ps1` 会自动校验 `config\version.json` 与 `common.ps1` 读数一致。

## 七、常见问题

- **UV4 构建超时**：关闭 Keil IDE 再跑（UV4.exe 与 IDE 共用进程名）。
- **构建速度**：全链路默认增量（Keil `-b` + `-j0` 并行、GCC ninja 只编改动文件）；
  改 `.h` 头文件会触发依赖文件重编属正常；确需全量加 `-Clean`。
- **verify 报 MISSING**：确认调试串口（默认 COM5，可用 `D00_DEBUG_PORT` 覆盖）是调试串口、波特率 115200；或调整 `VerifyExpect`。
- **flash 失败**：确认 SWD 线/供电/目标未占用；重试前先跑 `self_check.ps1 -TestHw`。
- **OTA 拒绝降级**：`-Version` 必须大于等于板上当前版本。
- **README 魔数地址过时**：以 `boot_config.h`（0x0805FFF8）和 `app_config.h`（0x0805FFFC）为准。

## 八、可继续扩展

- 将流水线封装为 Codex Skill，跨仓库复用；
- 日志关键字做成配置文件，覆盖更多启动场景。
