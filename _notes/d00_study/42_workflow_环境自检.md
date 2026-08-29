# D00 源码研学 · 笔记 42 —— 环境自检（self_check.ps1）

> 精读对象：`workflow/self_check.ps1`(184)
> 阶段 5 第 3 批：流水线的"安检门"——**任何固件改动后的第一道关卡**（AGENTS.md 第 4 节第 1 步）

---

## 一、八大检查域

| 域 | 内容 | 级别 |
| --- | --- | --- |
| 工具链 | Keil UV4 / STM32_Programmer_CLI / Python+pyserial / cmake / ninja / OpenOCD | FAIL（缺失） |
| 工程文件 | APP/BOOT .uvprojx / .sct / git 仓库 | FAIL |
| 串口 | DebugPort / HostPort 在线（换 USB 口漂移提醒） | WARN |
| **约定一致性** | app_config.h 含 0x080DFFFC（版本地址）+ 1MB（下载区）；boot_config.h 含 0x4F54412E（魔数）——**文档漂移检测** | WARN |
| **版本单一事实源** | config/version.json vs common.ps1 默认值必须一致 | FAIL |
| **发布安全** | APP.bin 不得含 "Crash injection" 后门字符串（崩溃注入命令仅调试构建） | FAIL |
| **源码编码** | 全部自有源（.c/.h/.py/.ps1/.md/.json）必须严格 UTF-8（`UTF8Encoding($false,$true)` 抛异常检测） | FAIL |
| **可复现性** | APP.ld / GCC toolchain / CI workflow / workflow 文件入 git | FAIL |
| 文档漂移 | README 关键约定（0x080DFFF8 / 832KB）与现状一致 | FAIL |

## 二、⭐ 设计要点

### 1. 约定一致性即"代码"（:49-60）
魔数/地址/大小这些**跨文件约定**用正则扫描验证——`0x080DFFFC`（APP 版本地址）、`0x4F54412E`（APP_VALID_MAGIC）、`1024*1024`（外部下载区）——**文档漂移在提交前暴露**。

### 2. 发布固件安全（:99-108）
```
APP.bin 字符串扫描 "Crash injection"：
  崩溃注入命令（crash bus|undef|stack|assert）仅 APP_DEBUG_MODE=1 编译进固件；
  发布构建必须无此字符串——后门扫描
```

### 3. 严格 UTF-8 编码（:110-144）
- `UTF8Encoding($false, $true)`：**严格模式**（非法字节抛异常）——比宽松解码更严
- 排除 build/__pycache__/MDK-ARM/Middlewares/Drivers（第三方）

### 4. 版本单一事实源（:82-96）
version.json vs common.ps1 不一致 → FAIL——**发版前强制同步**。

### 5. 三态结果
- `[OK]` / `[FAIL]`（计数 exit 1）/ `[WARN]`（计数 exit 0）——流水线按 exit code 判定

## 三、设计亮点

1. **约定即代码**：跨文件常量用扫描验证（防文档漂移）
2. **后门扫描**：发布固件无崩溃注入
3. **严格编码**：UTF-8 非法字节必现
4. **单一事实源强制**：version.json 唯一入口
5. **三态分级**：FAIL 阻塞 / WARN 提醒

## 四、待读清单（下一课——收官）

- [x] self_check（本轮完成）
- [ ] `auto_build.ps1` / `auto_flash.ps1` / `auto_verify.ps1` / `auto_hosttest.ps1`（快速扫读）
- [ ] **项目收官总结 + 总架构图**（最终输出）

## 五、自测题

1. 约定一致性检查哪三个常量？（0x080DFFFC / 0x4F54412E / 1MB）
2. 后门扫描找什么字符串？（Crash injection）
3. 严格 UTF-8 怎么实现？（UTF8Encoding 严格模式抛异常）
4. 版本不一致是什么级别？（FAIL）
5. 串口不在线是什么级别？（WARN——设备未插）
6. 三态结果怎么影响 exit code？（FAIL=1 / WARN=0）
7. 为什么排除 MDK-ARM/Middlewares？（第三方/生成物）
8. README 漂移检查什么？（0x080DFFF8 / 832KB）
