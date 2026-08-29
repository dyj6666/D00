# D00 源码研学 · 笔记 39 —— HOSTLINK 协议工具与其余 HOST 工具速览

> 精读对象：`HOST/OTA_Tool/core/hostlink.py`(102) + VLink_Debugger / LogicAnalyzer / EthLab / D00Term 结构
> 阶段 4 第 4 批：协议层 + 工具全景

---

## 一、⭐ hostlink.py（102 行）—— 与 APP protocol.h 逐字节对齐

| 元素 | HOST（Python） | 板端（C） | 一致点 |
| --- | --- | --- | --- |
| 同步字 | SYNC1/2 = 0xAA/0x55 | protocol.h | 相同 |
| 命令码 | 0x08~0x0D + 0x06 | CMD_OTA_* | **0x0C = BOOT 状态广播**（上位机可视化依据） |
| CRC | CRC-16/MODBUS（0xA001 位算法） | crc16.c | 相同 |
| 帧格式 | `AA 55 cmd len<LE payload crc<LE` | protocol.h | 相同 |
| 数据块 | OTA_CHUNK_MAX=240 | app_config.h | 相同 |
| 状态解析 | (state, rx, total) | data_link 响应布局 | 相同 |

**FrameParser（:75-102）**：流式解析 + **逐字节滑动重同步**（:86-88，坏字节 pop）+ CRC 校验——与 UartTransport 组合成"重试 4 次 + 重同步"的稳健接收。

## 二、其余 HOST 工具速览

### VLink_Debugger（~600 行）
- `vlink/client.py`(114)：HOSTLINK 客户端（连接/订阅/读写变量）
- `ui/main_window.py`(305) + `plot_widget.py`(155)：变量监视 + **实时波形图**
- 与 APP var_manager/HOSTLINK 协议对应（CMD_LIST_VARS/SUBSCRIBE/READ/WRITE）

### LogicAnalyzer（~1,900 行）
- `la/decoders.py`(481)：**协议解码器**（UART/SPI/I2C/CAN 等总线解码——上位机分析 LA 采样数据）
- `ui/waveform.py`(352)：波形显示（滚动/缩放/光标）
- `ui/main_window.py`(487) + detail_panel(214)
- `tests/test_decoders.py`(147) + test_robustness(191)：**解码器单测 + 硬件健壮性测试**
- 与 APP la_* 服务对应（CMD_LA_DUMP 数据源）

### EthLab（~1,300 行）
- `eth/decoders.py`(381)：以太网帧解码（EthLab 抓帧 :7778 数据）
- `ui/capture_panel.py`(220) + console_panel(188) + byte_view(143) + stats_panel(119) + echo_dialog(156)
- `tests/smoke_test.py`(107)：冒烟测试
- 与 APP eth_app 抓帧/UDP 回显对应

### D00Term（562 行）
- 通用串口终端（调试口 USART3 伴侣）

### DapTool（~1,650 行）
- `dap_core.py`(612)：**CMSIS-DAP 协议核心**（SWD 读写/调试）
- `dap_gui.py`(929)：GUI（寄存器/内存/Flash/断点）
- `probe_w25q128.py`(103)：直接探 W25Q128（BOOT/APP 调试利器）
- 与 AGENTS.md 第 9 节 DAP 调试约定对应（pclist/fault/read 寄存器）

## 三、设计亮点

1. **协议单一事实源**：hostlink.py 与 protocol.h 注释互相指向——双端契约文档化
2. **解码器库**：LogicAnalyzer/EthLab 的解码器可单测（CI 覆盖）
3. **工具矩阵完整**：烧录（DapTool）/ 升级（OTA_Tool）/ 调试（VLink）/ 分析（LA）/ 网络（EthLab）/ 终端（D00Term）——覆盖产品全生命周期
4. **测试配套**：每个工具都有 tests/（解码器/健壮性/冒烟）

## 四、待读清单（下一课——workflow 收官）

- [x] hostlink + HOST 全景（本轮完成）
- [ ] `config/version.json` + `version_lib.py`：版本单一事实源
- [ ] **workflow/ 流水线**：common.ps1 / auto_build / auto_flash / auto_verify / auto_ota / auto_pipeline / self_check
- [ ] 项目收官总结

## 五、自测题

1. FrameParser 怎么重同步？（逐字节滑动 + CRC 校验）
2. 0x0C 广播命令码的作用？（BOOT 升级状态可视化）
3. OTA_CHUNK_MAX=240 双端一致的意义？（协议契约）
4. VLink_Debugger 与 APP 哪个模块对应？（var_manager/HOSTLINK）
5. LogicAnalyzer 解码器测什么？（test_decoders）
6. EthLab 数据源是什么？（:7778 抓帧）
7. DapTool 与 workflow 的关系？（DAP 调试约定）
8. 各工具的测试配套？（解码器单测/健壮性/冒烟）
