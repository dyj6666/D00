# D00 源码研学 · 笔记 36 —— HOST 上位机总览与 OTA 引擎

> 精读对象：`HOST/OTA_Tool/core/ota_engine.py`(583) + HOST 目录全景
> 阶段 4 开始：上位机（Python/PyQt5，~9.7k 行）——**与 BOOT/APP 协议对接的完整闭环**

---

## 一、HOST 目录全景（5 工具 + 协议）

| 工具 | 行数 | 定位 |
| --- | --- | --- |
| OTA_Tool | ~2,300 | **安全升级工具**（三通道 + 加密签名 + 断点续传）——本轮 |
| VLink_Debugger | ~600 | 上位机调试器（HOSTLINK 变量/波形/订阅） |
| LogicAnalyzer | ~1,900 | 逻辑分析仪（LA 数据解码/波形显示/触发） |
| EthLab | ~1,300 | 以太网实验台（抓帧 :7778 / UDP 回显 :7777 / TCP 控制台 :9000） |
| D00Term | 562 | 通用串口终端 |
| DapTool | ~1,650 | CMSIS-DAP 硬件调试器（寄存器/内存/Flash） |

## 二、⭐ OTA 引擎（ota_engine.py）—— 一次 OTA 会话的驱动线程

### 2.1 架构
```
OtaEngine(QThread)：与界面完全解耦，可无头测试
  run() → _prepare_package（构建安全包）→ 按模式分发：
    uart+ymodem → YMODEM 传统升级（BOOT 直接收）
    uart        → HOSTLINK 逐帧确认
    tcp         → :9020 流水线（窗口可配）
    http        → 板端拉取（PC 起 HTTP 服务器 + UART/TCP 控制通道下发命令）
  → 升级后验证（BOOT 广播可视化 / 启动日志 / :8080 状态页）
```

### 2.2 ⭐ 固件包构建（_prepare_package :140-163）
```
uid（设备 UID）+ key（私钥）+ 固件 → derive_aes_key_from_uid → encrypt_and_sign
  → _ota_pkg.bin（AES-CTR 加密 + ECDSA 签名，与 BOOT security.c 六道关对应）
build_no 自动分配（version_lib 单设备自动；批量场景预分配防并发竞争）
```

### 2.3 三通道传输
| 模式 | 特点 | 关键数字 |
| --- | --- | --- |
| UART HOSTLINK | 逐块确认（**无固定 sleep，紧贴 ACK 节奏，速率拉满**） | 240B 块 / 921600 |
| TCP :9020 | **流水线窗口 8 块（2KB 突发）**：隐藏网络 RTT，实测 96KB/0.5s ≈ 190KB/s 逼近 Flash 写入极限；更大窗口偶发 ACK 停滞（板端小窗口/分段交互） | 窗口可配 |
| HTTP | PC 起 HttpOtaServer → 控制通道（UART shell / TCP :9000）下发 `ota http <ip>/<path>` → 解析板端 `[OTA-HTTP] x/y` 进度行 | :8081 服务 |

### 2.4 ⭐ 断点续传（三通道）
```
BEGIN 成功后 → STATUS 查询已收进度 → 从断点续传（UART/TCP）
TCP --no-resume：先发 RESET 强制从零开始
```

### 2.5 升级后验证三层
1. **BOOT 阶段广播可视化**（_on_boot_frame :255-269）：监听 CMD_OTA_BOOT_STATUS(0x0C)——7 阶段名映射（探测预下载包→安全校验→备份 RUN→擦除→解密写入→提交→完成重启）；phase=0xFF 失败即时显示
2. **启动日志抓取**（_verify_boot_log）：20s 窗口抓调试口，核验 `last build #N` + `Boot complete`
3. **:8080 状态页轮询**（_verify_http_status）：uptime<60s + link=1 确认刚重启；TCP 快速模式优先 :9020 STATUS（新固件 OTA 服务在线=在跑），失败才回退状态页

### 2.6 速率计量血泪（:563-577）
```
"必须用 perf_counter（QPC）：monotonic 在此平台是 GetTickCount64，
15.6ms 分辨率导致逐块 dt 恒为 0，速率/ETA 永远算不出来"
```
→ 瞬时速率平滑（0.8 旧 + 0.2 新）+ ETA。

## 三、设计亮点

1. **引擎与 UI 解耦**：QThread + 信号——可无头测试（CI）
2. **四通道全覆盖**：YMODEM/HOSTLINK/TCP/HTTP——与板端四通道一一对应
3. **流水线窗口**：RTT 隐藏 + 逼近 Flash 极限的工程调优（实测数据）
4. **验证闭环**：BOOT 广播 + 启动日志 + 状态页三层证据
5. **安全包构建**：UID 派生密钥 + ECDSA 签名 + build_no 防重放（与 BOOT 对称）

## 四、待读清单（下一课）

- [x] ota_engine（本轮完成）
- [ ] `transport.py`(234)：UART/TCP/HTTP 传输层
- [ ] `ymodem_sender.py`(368)：YMODEM 发送器 + 加密签名
- [ ] `hostlink.py`：协议帧构建/解析（与 APP protocol.h 对应）
- [ ] `main_window.py`(886)：GUI
- [ ] VLink_Debugger / LogicAnalyzer / EthLab

## 五、自测题

1. OTA 引擎为什么用 QThread？（UI 解耦 + 无头测试）
2. 固件包构建的密钥体系？（UID 派生 + 私钥签名，与 BOOT 对称）
3. TCP 流水线窗口 8 块的依据？（实测 190KB/s 逼近 Flash 极限）
4. 断点续传怎么实现？（BEGIN→STATUS→从断点续）
5. 升级后验证的三层是什么？（BOOT 广播/启动日志/状态页）
6. 速率计量为什么用 perf_counter？（monotonic 15.6ms 分辨率坑）
7. HTTP 模式的控制通道有哪些？（UART shell / TCP :9000）
8. BOOT 广播 0x0C 的 7 阶段名是什么？（探测→校验→备份→擦除→写入→提交→重启）
