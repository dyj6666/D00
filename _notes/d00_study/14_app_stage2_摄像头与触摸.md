# D00 源码研学 · 笔记 14 —— 摄像头链路与触摸服务

> 精读对象：`SystemServices/cam_link.c`(307) · `touch_svc.c`(254)
> 阶段 2 服务层第 10 批：两路输入传感（视觉手势 + 触摸）

---

## 一、⭐ 摄像头链路（cam_link.c）—— OpenART mini 视觉协处理器

**硬件**：UART5（PC12/PD2，115200）接 OpenART mini（OpenMV 类视觉模块）——**协处理器分工**：AI 视觉（手部检测/挥手/手势识别）跑在 OpenART，STM32 只做协议解析。

### 1.1 协议（CAMERA/docs/串口协议.md）
```
帧格式：AA 55 | TYPE | LEN | DATA | SUM（TYPE..DATA 累加和低 8 位）
TYPE 0x01 手部坐标帧（flags+x,y,w,h，LEN=9）
     0x02 挥手事件（gesture_id：01 LEFT / 02 RIGHT / 03 UP / 04 DOWN）
     0x03 AI 手势帧（gesture_id + confidence）
```

### 1.2 接收架构：IDLE+DMA 循环（零字节中断）
```
UART5 RXNE/DMA 循环模式持续写入 256B 缓冲（DMA1_Stream0/CH4）
  → IDLE 中断（一帧结束）→ CamLink_IdleISR → 消费 DMA 环形缓冲 → 协议状态机
  连续满速流（无帧间隙，IDLE 不触发）→ DMA 满回调 HAL_UART_RxCpltCallback 兜底消费
```

**协议状态机**（CamLink_OnRxByte :110-157）6 态：HEAD1 → HEAD2 → TYPE → LEN → DATA → SUM。
**防呆设计**（:131-138）：**非法长度（>16）直接失步重同步**——否则 s_rx_idx 到 16 后不再增长、永远等不到 SUM，解析器永久卡死（噪声/协议错帧可触发，挥手/统计全失效）。

### 1.3 ⭐ 三个血泪级设计

**1. DMA 缓冲必须放普通 SRAM 不能放 CCM**（:32-33）：CCM（0x10000000）仅 CPU 可访问，**DMA 写入无效且触发总线错误（曾致 LCD 卡死）**。

**2. 满回调必须消费"整圈"**（:270-275）：TC 触发时 NDTR 已重载为 256 且中断延迟内仅递减 0-1 字节（115200bps 每字节 87µs）→ cur≈0 → 每圈只消费 0-1 字节，99% 帧被覆盖丢弃（实测：连续满速流解析率仅 ~8fps vs 理论 880fps）。修复：先消费整圈，再消费 TC 之后新写入的余量。

**3. 链路巡检自愈**（:202-247）：HAL_UART_Receive_DMA 自动使能 PE/ERR 中断；帧错误（FE，线缆接触不良/拔插/悬空噪声）时 HAL 错误分支会清 CR3 DMAR 并 HAL_DMA_Abort——**UART5 不在 bsp_uart 表内，无人恢复，链路永久静默死**。巡检：曾收到帧但 last_rx_ms 超时（5s）→ 重启 DMA 接收（拔线场景每 5s 重启一次，开销微秒级，插回立即恢复）。
- 巡检挂**事件总线 1s 心跳**（:303-304）：与 ota_can_svc 同模式，**不额外建任务**

### 1.4 状态缓存
ISR 写 / 任务读（volatile）：hand_present/x/y/w/h、swipe_left/right **事件标志**（应用层消费后清零，CamLink_ConsumeSwipe）、gesture_id/conf、frame_count/err_count/idle_count/last_rx_ms。

## 二、触摸服务（touch_svc.c）—— 500Hz 采样 + 事件语义

### 2.1 事件状态机
```
NONE/UP ──(连续 2 次有效+消抖)──► DOWN（原始值，零延迟）
DOWN ──► MOVE（位移钳制 + 轻平滑）
DOWN/MOVE ──(触点消失)──► UP（仅一次，保持到下次按下）
```
**UP 保持语义**（:48-49）：UP 状态保持到下一次按下——UI 按代数（gen++）消费，不会重复触发；若立即复位 NONE 且无代数变化，UI 可能错过 UP 导致手势丢失。

### 2.2 信号处理链
| 环节 | 实现 |
| --- | --- |
| 消抖 | 连续 2 次有效判定按下（TSV_PROBE_N=2，8ms 空闲节拍） |
| 速度钳制 | 单采样位移 ≤20px（人手最快 ~12px/采样），超限视为噪声尖峰并钳制（保留方向） |
| 轻平滑 | `(新 + 7×旧) >> 3`——首次按下不滤波（响应零延迟），MOVE 才平滑 |
| 位移追踪 | max_dx/max_dy 全程最大位移（接触抖动/中途瞬断不影响手势判定） |

### 2.3 四角校准（TouchSvc_Calibrate :171-225）
- 屏显十字（TL/TR/BR/BL 四角）→ 等待按下-抬起（20s 超时）→ 采集物理 AD
- **轴对齐线性映射（支持各轴反向）**：`xfac = (Δrx_左右)/2/200`，中心偏移补偿（:201-209）
- 校准结果**存 EEPROM**（UsrStore_Set USR_KEY_TOUCH_CAL）；启动恢复（TouchSvc_Init :232-240）

## 三、设计亮点

1. **协处理器架构**：AI 视觉放 OpenART，MCU 只解析协议——算力分工清晰
2. **零字节中断接收**：IDLE+DMA 循环，高负载不丢帧
3. **自愈巡检挂心跳**：不额外建任务（事件总线订阅 1s tick）
4. **事件标志语义**：挥手事件消费后清零、UP 保持到下次按下——UI 不会丢事件
5. **触摸信号链**：消抖→钳制→平滑→位移追踪——触摸体验的工程细节
6. **校准持久化**：EEPROM 存校准参数，重启即恢复

## 四、待读清单（下一课——SystemServices 收尾）

- [x] cam_link / touch_svc（本轮完成）
- [ ] `ext_mem.c`(330)：外部 SRAM 统一内存池
- [ ] `net_config.c`(53) + `cmd_can.c`(105) + `crc16.c`(21)（小文件快扫）
- [ ] `test_ext_mem.c`(194)：内存池自检
- [ ] **SystemServices 完成 → 转 Application 网络服务批**（eth_app 706 / tcp_svc / icmp_svc / dns_svc / sntp_svc / mqtt_svc / http_svc）

## 五、自测题

1. DMA 循环缓冲为什么不能放 CCM？曾发生什么故障？
2. 满回调为什么必须消费"整圈"？用 256-NDTR 计算会怎样？（实测数据）
3. 链路"静默死"的根因是什么？为什么没人恢复？（提示：HAL 错误分支 + bsp_uart 表）
4. 巡检为什么挂事件总线心跳而不是建任务？
5. 触摸 UP 事件为什么保持到下次按下？立即复位会怎样？
6. 触摸位移钳制解决什么？轻平滑为什么"首次按下不滤波"？
7. 四角校准的轴对齐映射公式怎么理解？（xfac 推导）
8. cam_link 的协议状态机防呆设计防什么？（非法长度）
