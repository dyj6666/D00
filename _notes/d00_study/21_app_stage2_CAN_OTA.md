# D00 源码研学 · 笔记 21 —— CAN OTA 通道

> 精读对象：`Application/ota_can_svc.c`(253) + `ota_transport.h` 传输注册表
> 阶段 2 应用层第 6 批：CAN 总线 OTA——**8 字节帧协议翻译器**

---

## 一、定位（头注释 :4-5）

仅做 **"CAN 帧 ↔ OTA 下载核心"的协议翻译**——下载核心（Ota_Begin/Data/End）带互斥与断点续传，传输层零侵入。第四种 OTA 通道（UART HOSTLINK / TCP :9020 / HTTP 拉取 / CAN 1Mbps）。

## 二、CAN 帧协议（can_proto.h）

| ID | 用途 |
| --- | --- |
| 0x200 (CAN_OTA_CTRL_ID) | 控制帧：BEGIN/END/STATUS/ABORT |
| 0x201 (CAN_OTA_DATA_ID) | 数据帧：seq(3bit) + last(1bit) + data(≤7B) |
| 0x210 (CAN_OTA_REPLY_ID) | 应答：BEGIN 结果 / END 结果 / 状态 |
| CAN_OTA_ACK_ID | 块 ACK：OK/ERR + 已收字节（主机据此发下一块） |

**行帧规约**：240B 块拆成多帧 CAN（每帧 7B 数据）→ `s_stream` 组装 → 凑满一块 → `Ota_Data` 写下载区。

## 三、⭐ 关键设计

### 1. 数据流严格顺序（:153-206）
- `seq == s_stream_seq` 才接受；乱序 → **整块丢弃**（防总线故障污染固件包）
- 超长块（>240B）→ 丢弃
- 诊断计数 `s_dbg_seq_err` 限流打印（≤3 次防刷屏）

### 2. 重传幂等（:193-199）—— 精妙设计
```
Ota_Data 返回 2（offset 失配 = 块可能已被写入，主机 ACK 丢失后重传）
  → Ota_Status 对齐实际进度 → 回 ACK(已收字节)
  → 重传幂等：不会因 offset 失配级联失败
```

### 3. ⭐ 严格 ID 过滤（:208-222）—— 血泪注释
```
"关键：数据流必须严格限定 0x201——总线上的应答回显帧（0x210/0x101）若被当作
 数据帧会以 data[0]=0x84 等伪序号污染流状态，导致后续块全部丢弃。"
```
→ CAN 是共享总线，**自己的应答也会被自己收到**（回显）——必须按 ID 精确分流，否则伪序号污染流状态。

### 4. 无数据超时自动 ABORT（:225-237）
- 5s 无数据 → Ota_Reset 清会话回 IDLE——**下载区不悬挂占用**
- **挂 1s 心跳不建任务**（与 cam_link 同模式）：并入 eventBusTask 的 MSG_TICK_1S，省一个任务与 512B 栈

### 5. END 冲刷余量（:100-123）
- 末尾不足一块的余量在 END 时冲刷（仅允许最终块出现）
- 数据未收齐（Ota_End 返回非 0）→ **保持会话活动**，等待主机补齐后重发 END

### 6. 传输注册表（ota_transport.h + OtaMgr）
```c
ota_transport_t { id, name, desc, available }
OtaMgr_Register / OtaMgr_Get / OtaMgr_Count —— `ota status` 命令列出全部传输
```
→ UART/TCP/HTTP/CAN 四通道统一注册，`ota status` 可查（笔记 07 cmd_ota）。

## 四、设计亮点

1. **传输零侵入**：四通道共用 ota_agent 下载核心（互斥/续传/校验统一）
2. **重传幂等**：ACK 丢失场景自对齐，不级联失败
3. **回显污染防护**：共享总线场景的 ID 精确分流
4. **空闲超时监管**：心跳复用不建任务
5. **限流打印**：诊断计数防刷屏

## 五、待读清单（下一课——GUI 批）

- [x] ota_can_svc（本轮完成）
- [ ] `gui_app.c`(351) / `gui_pages.c`(1876) / `gui_theme.c`：LVGL GUI
- [ ] `data_agent.c` / `buzzer_app.c` / `key_app.c` / `led_app.c` / `cmd_can.c`

## 六、自测题

1. CAN OTA 的 240B 块怎么拆帧？seq/last 位的作用？
2. 为什么"自己的应答帧"会污染数据流？怎么防护？（ID 过滤）
3. Ota_Data 返回 2 时为什么是"重传幂等"？怎么对齐进度？
4. 无数据超时 5s 自动 ABORT 解决什么？（下载区悬挂）
5. 监管为什么挂 1s 心跳不建任务？（省任务与栈）
6. END 时数据未收齐会怎样？（保持会话，等待补齐重发 END）
7. 四通道 OTA 共用了什么？（ota_agent 下载核心）
8. `ota status` 命令怎么列出各传输？（OtaMgr 注册表）
