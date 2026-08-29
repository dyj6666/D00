# D00 源码研学 · 笔记 08 —— OTA 代理（APP 侧下载与启动确认）

> 精读对象：`Application/ota_agent.c`(548)
> 阶段 2 核心：APP 侧 OTA 全生命周期——BEGIN/DATA/END → 复位 → BOOT 校验安装 → 启动确认

---

## 一、OTA 全景（APP ↔ BOOT 职责划分）

```
                     APP 侧（本文件）                    BOOT 侧（笔记 01/02）
┌──────────────────────────────────────┐   ┌──────────────────────────────────────┐
│ 下载（写外部 ota_dl 槽）               │   │ 校验+安装（读外部 ota_dl 槽）           │
│  · 多传输：UART/TCP/HTTP/CAN          │   │  · verify_and_decrypt 六道安全关        │
│  · 断点续传会话槽（内部 PARAM 区尾部）  │   │  · 备份 RUN → img_lib                  │
│  · 互斥锁防并发传输                    │   │  · 解密写 RUN + 向量校验                │
│  · 参数区 UPGRADE + BKP 双保险触发     │   │  · 置 PENDING → 复位                   │
│ 启动确认（新固件首次运行）               │   │  · 回滚/防重放                        │
│  · PENDING → NORMAL（清计数）          │   │                                      │
│  · 失效外部备份槽头（防旧备份复活）      │   └──────────────────────────────────────┘
└──────────────────────────────────────┘
```

**版本检查是双层防线**：APP 侧 Ota_Begin 预检（version ≥ 当前，:246-254）→ BOOT 侧 security.c 二次校验（防绕过）。

## 二、状态机与数据结构

```c
OTA_ST_IDLE(0) → Ota_Begin → OTA_ST_RECEIVING(1) → Ota_End → OTA_ST_DONE(2)
```

**两个持久化结构**（与 BOOT 严格一致）：
1. **参数区** `ota_param_t`（28B，:29-39）：magic 'PRMT' / boot_state / boot_count / rollback_count / last_error / **last_build_no（防重放）** / crc32——与 BOOT/boot_param.c 同布局
2. **会话槽** `ota_session_t`（20B，槽距 32B，:42-50）：magic 'OTAM' / version / total / **received（续传起点）** / crc32——1024 槽位于 PARAM 扇区空余 0x080E2000

## 三、⭐ 四个关键设计

### 1. 断点续传（:259-274 + :330-338）
- **Ota_Begin 自动检测**：存在同版本同大小的部分会话 → 不擦下载区，`ota_received = sess.received` 从断点继续；并 `BSP_Flash_ResetController()` 重置 Flash 控制器状态（防残留导致编程 BSY 卡死）
- **进度持久化节流**：每 16 块（3840B）写一次会话槽——每块省 ~1ms Flash 写，三通道整体提速；断点粒度 3840B 可接受；**恢复点 = 实际已写位置**，避免重写已写区域导致 Flash 编程失败
- 会话槽写失败：魔数写 0（1→0 无需擦除）；`ota_session_clear` **跳过已擦除槽**（全 0xFF 判断）——整区清除从 ~300ms 降到微秒级（Ota_Reset 可安全在事件总线执行）

### 2. 传输互斥锁（:57-76）
任何传输调用 Ota_* 前必须 `ota_mutex_take()`（200ms 超时），失败**必须放弃操作并报错**——注释明确："严禁'超时仍进临界区'的既往行为"（曾经的 bug 教训）。防 UART/TCP/HTTP 并发写下载区。

### 3. 双保险触发升级（:373-395）
```
Ota_End：参数区写 UPGRADE（独立状态不会误判回滚） + BKP_DR1 写 0x5A5A → 复位
BOOT 决策树两分支都能进升级模式（笔记 01 第四节的 1)/2) 两条路径）
```

### 4. 启动确认闭环（:184-219）
```
BOOT 安装后置 PENDING → 新固件首次运行 OtaAgent_Init → ota_confirm_startup：
  · 参数有效且 PENDING → 写 NORMAL + boot_count=0（双份写入）
  · 擦除外部备份槽头 4KB（img_lib）★：BOOT 升级时保留备份直至本确认点，
    PENDING 期间回滚源可用；确认成功即擦头令 OtaBackup_IsValid 失效——杜绝旧备份复活
  · Buzzer_OtaSuccess() 播"三短一长"完成旋律
```

## 四、HOSTLINK OTA 通道（data_link_ota_handler，:458-527）

| 命令 | 请求 payload | 响应 |
| --- | --- | --- |
| CMD_OTA_BEGIN | version(u32) + size(u32) | 1B 状态码 |
| CMD_OTA_DATA | offset(u32) + data(≤240B) | 1B 状态 + 4B 已收字节（进度回显） |
| CMD_OTA_END | — | 1B 状态码 |
| CMD_OTA_STATUS | — | 1B 状态 + 4B 已收 + 4B 总长 |
| CMD_OTA_RESET | — | 1B 状态码 |

**调用方约束**（头注释）：必须顺序写、不跳块（`offset != ota_received` 拒绝）；Flash 编程期间关中断。

## 五、危险自测（ota_rbtest → Ota_ForceRollbackTest，:428-453）

参数区置 `PENDING + boot_count=3`（= BOOT MAX_BOOT_TRIES）→ 复位 → BOOT 立即回滚——**端到端验证回滚链路**。仅 UART 通道可用（远程禁止）。

## 六、设计亮点

1. **双层版本防线**：APP 预检 + BOOT 强校验——攻击面最小化
2. **断电续传**：会话槽每 3840B 持久化，断电后从断点续传（不重写已写区）
3. **备份槽确认点失效**：PENDING 期间回滚源可用，确认成功即擦头——防旧备份复活的时序设计
4. **互斥锁纪律**：取锁失败必须放弃，绝不"超时仍进临界区"
5. **蜂鸣器全流程提示**：开始滴 / 失败三短 / 下载完成滴滴 / 确认成功三短一长 / RECOVERY 三短
6. **恢复模式提示**：参数区 RECOVERY 状态 → Buzzer_OtaFail 提示人工介入

## 七、待读清单（下一课）

- [ ] `ext_store.c`(478)：外部 Flash 分区存储实现（OTA_DL/IMG_LIB 分区表）
- [ ] `ota_tcp_svc.c`(261) + `ota_http_svc.c`：TCP/HTTP OTA 传输服务
- [ ] `usr_store.c`(378)：用户存储
- [ ] `sysmon.c`(228)：系统监控

## 八、自测题

1. APP 与 BOOT 各负责 OTA 的哪些阶段？版本检查为什么是双层？
2. 断点续传怎么知道"从哪续"？为什么不重写已写区域？
3. 会话槽进度为什么每 16 块才写一次？直接每块写会怎样？
4. ota_session_clear 为什么能跳过已擦除槽？判断条件是什么？
5. 启动确认时为什么擦外部备份槽头？不擦会怎样？
6. Ota_End 用 UPGRADE + BKP 双保险触发的意义？（提示：BOOT 决策树两条路径）
7. Ota_Data 的 `offset != ota_received` 检查防什么？
8. Ota_ForceRollbackTest 的原理？为什么仅 UART 通道可用？
