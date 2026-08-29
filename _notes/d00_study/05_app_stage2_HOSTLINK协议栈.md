# D00 源码研学 · 笔记 05 —— HOSTLINK 上位机协议栈

> 精读对象：`SystemServices/protocol.c`(77) + `.h`(102) · `data_link.c`(422) + `.h`
> 阶段 2 核心：APP ↔ 上位机的唯一通道（USART1，物理 CH340/921600）

---

## 一、帧格式（protocol.h:15-24，全部字段小端）

```
[0]     SYNC1      0xAA
[1]     SYNC2      0x55
[2]     CMD        命令码
[3..4]  payload_len  uint16 LE（不含帧头与 CRC）
[5..]   payload
[last2] CRC16-LE    （MODBUS poly 0xA001，对"帧头+payload"全部字节计算）
```

- 帧头 5B + CRC 2B，单帧最大 `HOSTLINK_TX_FRAME_MAX = 256B`（含 CRC）
- **纯逻辑层无硬件依赖**：主机单测覆盖 74 项断言（协议 CRC/长度/同步字边界）

## 二、命令码表（protocol.h:36-48）

| 命令 | 码 | 方向 | 说明 |
| --- | --- | --- | --- |
| CMD_LIST_VARS | 0x01 | 上位机→APP | 枚举全部注册变量 |
| CMD_SUBSCRIBE | 0x02 | 上位机→APP | 订阅变量（周期上报，默认 10ms） |
| CMD_DATA | 0x03 | APP→上位机 | **周期数据帧**（订阅推送） |
| CMD_READ_VAR / WRITE_VAR | 0x04/0x05 | 双向 | 读/写变量（ID u16 LE） |
| CMD_GET_INFO | 0x06 | 上位机→APP | 版本信息（协议版本 + 固件 major/minor/patch） |
| CMD_LA_DUMP | 0x07 | 双向 | 逻辑分析仪采样导出（大块，背压式多帧） |
| CMD_OTA_BEGIN/DATA/END/STATUS | 0x08-0x0B | 双向 | OTA 会话（**透传给 OtaAgent**） |
| CMD_OTA_RESET | 0x0D | 上位机→APP | 强制复位 OTA 会话（清下载会话槽） |
| CMD_ERROR | 0xFE | APP→上位机 | 错误响应：原命令码 + proto_err_t |

错误码 `proto_err_t`（10 种）：BAD_SYNC / TOO_SHORT / CRC / BAD_PAYLOAD_LEN / UNKNOWN_CMD / VAR_NOT_FOUND / VAR_READONLY / BUF_TOO_SMALL / NO_MEM。

## 三、协议层三函数（protocol.c，77 行纯逻辑）

| 函数 | 作用 | 校验 |
| --- | --- | --- |
| `Protocol_ValidateFrame` | 校验完整帧（含 CRC） | SYNC → 最短长度 → **CRC 全帧** |
| `Protocol_ParseHeader` | 解析帧头（不含 CRC） | SYNC → 长度 → **payload_len 声明==实际** |
| `Protocol_BuildFrame` | 组装完整帧 | 容量检查 → 写头/payload → 算 CRC |

## 四、⭐ DataLink 链路层（data_link.c，422 行）

### 4.1 双任务 + 双队列架构

```
上位机 ⇄ USART1（DMA 全双工）
        │
RX DMA ─► data_link_rx_isr（ISR 上半部）
        │    ① 整帧校验（CRC）——失败则滑动重同步（找 0xAA55）
        │    ② 校验通过 → 帧数据拷入 cmd_packet_t → cmd_queue（16 深）
        ▼
   CmdTask（DL_CMD, Low, 1KB 栈）── handle_command 分发
        │    · 变量类命令 → var_manager
        │    · LA_DUMP → la_sample 导出（背压发送）
        │    · OTA 命令 → s_ota_handler 透传（由 OtaAgent 注册）
        ▼
   DataLink_SendFrame* → tx_queue（8 深，整帧按值拷贝）
        ▼
   TXTask（DL_TX, Low, 1KB 栈）── memcpy 到 DMA 缓冲 → BSP_UART_TransmitDMA
        │    等待任务通知（DMA 完成 ISR → vTaskNotifyGiveFromISR）
        │    2s 超时 → AbortTransmit 自愈 + g_tx_err++
        ▼
   上位机
```

### 4.2 五个可靠性设计（面试素材）

1. **整帧队列保帧边界**（tx_frame_t 按值入队 256B）：DMA 发送不跨帧拼接，防大块截断——帧头+payload+CRC 一次搬运
2. **RX 滑动重同步**（:154-170）：CRC 失败时逐字节滑动找 `0xAA55` 再验——从残留字节恢复合法帧，抗串口噪声粘包
3. **TX 超时自愈**（:107-116）：等 DMA 完成通知 2s 超时 → Abort + 清残留通知 + `g_tx_err++`——任何丢通知/中止/错误都不永久挂起
4. **显式丢弃语义**（:197-213）：`DataLink_SendPacket` 非阻塞，队列满返回 -2 并计数 `g_tx_lost`——调用方绝不误判发送成功；可靠投递用 `SendFrameWait`（背压阻塞，用于 LA_DUMP 大块导出，与 921600 波特率排空速度自匹配，**绝不静默丢帧**）
5. **OTA 命令透传**（:404-416）：数据链路层不感知 OTA 协议细节，`s_ota_handler` 由 OtaAgent 注册——**依赖方向保持 Service 不向上依赖 Application**（虽然本文件在 Service 层，OTA 在 Application 层，靠注册回调解耦）

### 4.3 资源账

| 资源 | 大小 | 说明 |
| --- | --- | --- |
| rx_dma_buf | 256B（aligned 4） | RX DMA 常驻环形缓冲 |
| tx_dma_chunk | 256B（aligned 4） | TX DMA 搬运缓冲（单帧复用） |
| tx_frame_t 队列 | 8 × 258B ≈ 2KB | 整帧队列 |
| cmd_packet_t 队列 | 16 × 258B ≈ 4KB | 命令队列 |
| 栈 | 2 × 1KB | DL_TX + DL_CMD |

> ⚠️ cmd_packet_t.data 是 256B 定长数组（= RX DMA 缓冲大小），意味着**单帧 payload 上限实际受 DMA 缓冲约束**：HOSTLINK_TX_FRAME_MAX 256B 含 CRC → payload ≤ 249B。OTA_CHUNK_MAX=240 正是为此预留余量。

## 五、与 BOOT 的呼应

- **同一条 USART1**：BOOT 用 RXNE 中断 + FIFO（YMODEM 下载），APP 用 DMA 全双工（HOSTLINK）——ymodem_port_init 的"先停 DMA 再切中断模式"保证 BOOT→APP 干净交接
- **帧校验一致哲学**：BOOT YMODEM 帧 CRC32、APP HOSTLINK 帧 CRC16-MODBUS——各自协议域内自洽
- **0x0C 升级状态广播**（笔记 01）：BOOT 升级时用 HOSTLINK 帧格式发状态，与 CMD_OTA_STATUS(0x0B) 呼应（BOOT 侧独立实现，仅帧格式借用）

## 六、待读清单（下一课）

- [ ] `var_manager.c` + `var_list.c`：变量注册表（HOSTLINK 数据源，64 变量上限）
- [ ] `logger.c` + `shell.c` + `cmd_shell.c`：日志流与 Shell 命令体系
- [ ] `watchdog.c` + `err_mgr.c`(562)：任务级看门狗与崩溃管理
- [ ] `ota_agent.c`(522)：OTA 代理（会话槽/断点续传/HOSTLINK OTA 通道）
- [ ] `bsp_uart.c`：BSP_UART 接口实现（DMA 环形 + 半满/满中断 + IDLE）

## 七、自测题

1. HOSTLINK 单帧 payload 上限是多少字节？由哪三个约束共同决定？
2. RX 收到一帧 CRC 错误的垃圾数据，链路层会怎么处理？（提示：滑动重同步）
3. TXTask 为什么用任务通知而不是信号量？2s 超时自愈解决什么问题？
4. `DataLink_SendPacket` 与 `SendFrameWait` 的语义差别？什么场景必须用后者？
5. OTA 命令为什么不在 data_link 里直接处理？注册回调有什么好处？
6. 为什么 cmd_packet_t.data 必须 ≥ HOSTLINK_RX_DMA_BUF_SIZE？如果小于会怎样？
7. CMD_DATA(0x03) 是谁发的？触发条件是什么？（提示：订阅 + 周期采样）
