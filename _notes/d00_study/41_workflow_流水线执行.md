# D00 源码研学 · 笔记 41 —— 流水线执行器与 OTA 冒烟

> 精读对象：`workflow/auto_pipeline.ps1`(111) · `auto_ota.ps1`(67)
> 阶段 5 第 2 批：一键总流水线 + 端到端 OTA 验证

---

## 一、⭐ auto_pipeline.ps1 —— 一键总流水线

### 1.1 阶段编排（8 模式 + Skip 开关）
```
full:  selfcheck → build → flash → verify → hosttest（OTA 需 -IncludeOta）
quick: build → verify → hosttest
ota:   ota（仅）
每种模式只关闭"不属于本模式"的阶段；-Skip* 开关始终生效（先于 switch 计算）
```

### 1.2 ⭐ Run-Stage 独立子进程（:40-59）—— 关键设计
```
"独立子进程执行：ExitCode 是阶段脚本 exit 的精确值，不受 $?/$LASTEXITCODE
被内部原生进程（python 抓串口、OpenOCD 复位、空 catch 块）污染的干扰。"
```
→ 每个阶段 `powershell -File` 子进程 + `-Wait -PassThru`——**PS 环境变量污染免疫**。

### 1.3 报告（last_report.json）
- 每阶段：OK / FAIL(code) + 耗时秒
- 制品哈希：BOOT.bin / APP.bin 的 bytes + **sha256**（可复现性审计）
- git HEAD + 时间戳；summary PASS/FAIL

## 二、⭐ auto_ota.ps1 —— 端到端 OTA 冒烟

### 2.1 流程
```
HOSTLINK 模式：ota_hostlink_cli.py --no-resume APP.bin v b PORT
TCP 模式：     ota_tcp_cli.py APP.bin v b IP
并行：Com9Logger 抓调试口 35s（升级全程日志证据）
```

### 2.2 判定（:41-46）
```
otaOk = CLI exit 0 且 stdout 无 "FAILED|no response|err=[1-9]|state=[2-9]"
v2 = Test-BootLog（调试口日志：期望标记 + 失败标记 + 崩溃语义）
pass = otaOk AND v2.Pass
```

### 2.3 报告
- ota_download / ota_verify 两阶段
- **ota_recovered_crash**：历史崩溃恢复记录只提醒、不计失败（AGENTS.md 第 4 节）
- ota_host_tail：关键行尾部（OTA|BOOT|phase|FAIL）

## 三、设计亮点

1. **子进程隔离**：PS 环境变量污染免疫（$?/$LASTEXITCODE 被原生进程污染）
2. **双证据**：CLI stdout + 调试口日志——下载成功 ≠ 升级成功
3. **崩溃语义**：历史恢复提醒 vs 活动崩溃失败
4. **制品哈希**：sha256 审计（可复现性）
5. **日志证据链**：ota_log + ota_debug_log + host_tail 全部入报告

## 四、待读清单（下一课——收官）

- [x] auto_pipeline / auto_ota（本轮完成）
- [ ] `auto_build.ps1` / `auto_flash.ps1` / `auto_verify.ps1` / `auto_hosttest.ps1` / `self_check.ps1`（快速扫读）
- [ ] **项目收官总结 + 总架构图**（最后输出）

## 五、自测题

1. Run-Stage 为什么用独立子进程？（$?/$LASTEXITCODE 污染）
2. full 模式默认包含 OTA 吗？（需 -IncludeOta）
3. otaOk 的失败正则是什么？（FAILED|no response|err=[1-9]|state=[2-9]）
4. 历史崩溃恢复为什么不计失败？（AGENTS.md 语义）
5. 报告里制品哈希的作用？（可复现性审计）
6. 双证据判定？（CLI stdout + 调试口日志）
7. -Skip* 与模式的关系？（Skip 先于 switch 计算）
8. 每个阶段的耗时怎么记录？（report.stages[name]_sec）
