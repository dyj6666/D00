# D00 源码研学 · 笔记 40 —— workflow 自动化流水线（总览 + common.ps1）

> 精读对象：`workflow/common.ps1`(400) + 脚本全景（28 个文件）
> 阶段 5 开始：全自动开发闭环——**改代码→构建→烧录→日志验证→OTA→单测→提交**（AGENTS.md 第 4 节）

---

## 一、脚本全景

| 脚本 | 行数 | 职责 |
| --- | --- | --- |
| **common.ps1** | 400 | 共享配置 + 工具函数（本轮） |
| auto_pipeline.ps1 | 104 | **一键总流水线**（-Mode full 串联全部阶段） |
| auto_build.ps1 | 106 | 构建（Keil 增量 -b / GCC cmake+ninja） |
| auto_flash.ps1 | 81 | SWD 烧录（BOOT + 带魔数 APP 镜像） |
| auto_verify.ps1 | 63 | 复位后抓 COM 日志验证启动 |
| auto_ota.ps1 | 61 | HOSTLINK 安全升级冒烟 |
| auto_hosttest.ps1 | 82 | 主机单测（BOOT host_tests + APP 协议测试） |
| self_check.ps1 | 169 | **环境自检**（工具链/串口/编码/后门扫描/版本一致性） |
| dap_debug.py | 628 | DAP 硬件调试（pclist/fault/断点） |
| flash_dap.ps1 / flash_recover.py / chunked_flash.py | 361 | DAP 烧录与恢复 |
| watch_mcu.py / watch_heartbeat.py / catch_crash.py | 477 | 日志监控/心跳/崩溃捕获 |
| make_docs.py / make_ota_doc.py | 462 | 文档生成 |
| gen_wav.py | 131 | WAV 生成（wav_data.c） |
| ota_tcp_push.ps1 | 32 | TCP OTA 推送 |

## 二、⭐ common.ps1 —— 工具链与配置（:12-58）

| 工具 | 路径 |
| --- | --- |
| Keil | D:\MDK\CORE\UV4\UV4.exe |
| 烧录 | STM32_Programmer_CLI（+ OpenOCD 0.12 DAP） |
| Python | D:\Python\python.exe |
| 构建 | cmake + ninja + arm-none-eabi（GCC 交叉） |
| 分区 | BOOT 0x08000000 扇区 0-3 / APP 0x08010000 扇区 4-10（与 boot_config.h 一致） |

**版本单一事实源**（:44-58）：`config/version.json`（ota_version=213, ota_build=9213）→ 覆盖 fallback 默认值——**发版前必须保持两者一致**。

## 三、⭐ 核心工具函数（血泪级）

### 1. Test-KeilLog 数字解析（:183-196）—— 重大事故教训
```
"不能用 -match '0 Error(s)' 判定成功——'30 Error(s)' 含子串 '0 Error(s)'
会误判 OK（2026-08-19 实测：RAM 溢出 30 Error 被判成功，导致 2 轮 OTA
推送旧固件）。必须解析数字比较。"
```
→ 正则提取 `(\d+) Error(s)` 数字 == 0 才 OK——**构建判定错误的完整事故链**（误判→旧固件→2 轮 OTA）。

### 2. Invoke-Exe（:109-153）
- ProcessStartInfo 重定向 + **异步 ReadToEndAsync**（防输出管道死锁）
- 超时 Kill + 显式失败
- 参数含空格自动加引号

### 3. Invoke-UV4（:155-181）
- 默认增量 `-b`；-Rebuild 全量 `-r`
- **构建前检查 Keil IDE 是否打开**（超时提示）

### 4. 串口自动探测（:212-250）
- 调试口：env > 已配置在线 > **首个可用串口** > COM5（换 USB 口后 COM 号漂移自动适配）
- HOST 口：排除调试口的第一个端口

### 5. Reset-Target 统一复位（:297-318）
```
DAP（OpenOCD）→ ST-Link（CubeProgrammer）→ 串口 reset 命令 → 显式失败
```

### 6. Test-ProjectNoBom（:320-333）
- **BOM 检测**：项目/配置文件带 BOM 会被 Keil 静默拒绝（UTF-8 BOM 检查）

### 7. Test-StaleObjects（:335-361）
- **增量构建陈旧检测**：关键配置头（app_config/FreeRTOSConfig/lv_conf/main.c/uvprojx）比 .o 新 → 建议全量重建（Keil -b 不追踪全局头依赖）

### 8. ⭐ Test-BootLog（:370-399）—— 崩溃日志语义
```
历史崩溃恢复块（[CRASH] Previous crash recovered ...）是信息不是失败——先剥离
[CRASH] 行再扫坏标记；活动态崩溃单独检测（^\[CRASH\] 开头 + 非 recovered）
VerifyExpect：Modules initialized / ETH app ready / OTA Agent ready
VerifyFail：HardFault/UsageFault/assert/FATAL/SELF-TEST FAILED/SPOT CHECK FAIL/WritePixels OOB
```

## 四、设计亮点

1. **版本单一事实源**：version.json 唯一入口（self_check 校验一致性）
2. **构建判定数字解析**：子串匹配事故的完整教训
3. **串口自动探测**：COM 号漂移免疫
4. **崩溃日志语义化**：历史恢复 vs 活动崩溃区分
5. **BOM/陈旧检测**：Keil 工程卫生
6. **统一复位链**：DAP→ST-Link→串口三级回退

## 五、待读清单（下一课——流水线收官）

- [x] common.ps1（本轮完成）
- [ ] `auto_pipeline.ps1` / `auto_build.ps1` / `auto_flash.ps1` / `auto_verify.ps1` / `auto_ota.ps1` / `auto_hosttest.ps1` / `self_check.ps1`
- [ ] **项目收官总结 + 总架构图**

## 六、自测题

1. Test-KeilLog 为什么必须数字解析？（30 Error 含 0 Error 子串事故）
2. 版本单一事实源在哪？（config/version.json）
3. 串口探测的优先级？（env > 配置 > 首个 > COM5）
4. Reset-Target 的回退链？（DAP→ST-Link→串口）
5. BOM 检测防什么？（Keil 静默拒绝工程）
6. Test-StaleObjects 检测什么？（增量构建陈旧）
7. 崩溃日志怎么区分历史/活动？（recovered 行 vs 非 recovered 头）
8. Invoke-Exe 为什么异步读输出？（管道死锁防）
