# D00 源码研学 · 笔记 27 —— BSP 层总览与 UART 抽象

> 精读对象：`BSP/bsp_uart.c`(178) + BSP 目录全景
> 阶段 3 开始：BSP 驱动层（16 模块，6,012 行）——APP 与 HAL 之间的硬件抽象

---

## 一、BSP 层全景（6,012 行，16 模块）

| 模块 | 行数 | 用途 | 笔记 |
| --- | --- | --- | --- |
| LCD/lcd_ex.c | 1,568 | 液晶扩展（字库/绘图） | [28] |
| LCD/lcd.c | 1,195 | LCD 基础驱动 | [28] |
| bsp_w25q128.c | 526 | 外部 NOR Flash（与 BOOT esp_flash 对照） | [28] |
| bsp_lcd.c | 444 | LCD BSP 封装 | [28] |
| bsp_can.c | 313 | CAN1 1Mbps | [29] |
| bsp_eeprom.c | 281 | AT24C02（软 IIC） | [29] |
| bsp_es8388.c | 246 | 音频编解码器 | [29] |
| bsp_touch.c | 242 | 触摸屏 | [29] |
| bsp_sram.c | 226 | 外部 SRAM 自检+基准 | [29] |
| bsp_mpu6050.c | 187 | IMU 传感器 | [29] |
| bsp_i2s.c | 161 | I2S2 音频 | [29] |
| bsp_uart.c | 158 | **UART 抽象（本轮）** | [27] |
| bsp_power.c / system.c / watchdog.c / buzzer.c / gpio.c / flash.c / rtc.c | ~700 | 杂项 | [29] |

## 二、⭐ BSP_UART（bsp_uart.c）—— DMA 收发/空闲断帧/回调分发

### 2.1 通道表
```c
static bsp_uart_chan_t g_uart[BSP_UART_COUNT] = {
    [BSP_UART_DBG]  = { .huart = &huart3 },   /* 调试口 USART3 (PC10/PC11) */
    [BSP_UART_HOST] = { .huart = &huart1 },   /* HOSTLINK 口 */
};
```
每通道：rx/tx 回调 + 上下文 + 忙标志 + 延迟标志。

### 2.2 ⭐ 三个关键设计（全是血泪）

**1. TX 忙时推迟 RX 处理**（:125-131 + :156-159）
```
IDLE 中断到来但 TX 进行中 → rx_deferred=1 立即返回
TX 完成回调 → 先处理被推迟的 RX 空闲事件（此时 TX 已结束，DMAStop 安全）
```
→ 若不推迟：`HAL_UART_DMAStop`（RX 处理的第一步）会**误中止进行中的 TX DMA**，中止后无完成回调，TXTask 将永久挂起。

**2. 错误回调只清 TX 忙标志**（:166-177）
```
仅当 TX 传输因错误被终止（HAL 已将 gState 复位为 READY）才唤醒发送任务；
RX 错误（过载/帧错误）不影响 TX，不得误清忙标志导致并发 DMA。
```
→ `gState != BUSY_TX` 判定——**RX 错误不清 TX 标志**，防并发 DMA。

**3. 空闲断帧 + DMA 环形**（bsp_uart_process_rx :101-114）
```
IDLE → HAL_UART_DMAStop → count = rx_len - DMA 剩余 → 回调（数据+长度）
→ 若 rx_started 则重新启动 DMA 接收
```
→ **停-取-启**模式：帧边界由 IDLE 中断划定（一帧结束），回调拿到完整帧。

### 2.3 其他
- `BSP_UART_TransmitDMA`：tx_busy 互斥（忙则拒绝，防并发 DMA）
- `BSP_UART_AbortTransmit`：同步中止 + 复位忙标志（TX 状态自愈）
- `__weak BSP_UART_OnTxComplete`：非 BSP 通道（信号发生器 USART6）扩展钩子
- **IRQHandler 统一入口**（:134-139）：`HAL_UART_IRQHandler + IdleISR`——中断处理归一

## 三、设计亮点

1. **TX/RX DMA 互斥**：忙标志 + 推迟处理——同一条 UART 全双工 DMA 的正确姿势
2. **错误分类处理**：TX 错误唤醒 / RX 错误不清标志
3. **停-取-启模式**：IDLE 断帧 + 计数读取——帧边界精确
4. **弱符号扩展**：非 BSP 通道（USART6 信号发生器）通过 __weak 钩子接入

## 四、待读清单（下一课——BSP 主批）

- [x] bsp_uart（本轮完成）
- [ ] `bsp_w25q128.c`(526)：与 BOOT esp_flash 对照
- [ ] `bsp_can.c`(313) / `bsp_eeprom.c`(281) / `bsp_sram.c`(226)
- [ ] `bsp_es8388.c`(246) / `bsp_i2s.c`(161)：音频链
- [ ] `bsp_touch.c`(242) / `bsp_mpu6050.c`(187)：传感输入
- [ ] `bsp_power.c` / `bsp_system.c` / `bsp_watchdog.c` / `bsp_buzzer.c` / `bsp_gpio.c` / `bsp_flash.c` / `bsp_rtc.c`

## 五、自测题

1. TX 忙时收到 IDLE 为什么推迟 RX 处理？直接处理会怎样？（DMAStop 误中止 TX）
2. 错误回调为什么区分 TX/RX 错误？（防并发 DMA）
3. 空闲断帧的"停-取-启"三步是什么？
4. `gState != BUSY_TX` 判定的意义？
5. __weak BSP_UART_OnTxComplete 给谁用？（信号发生器 USART6）
6. tx_busy 标志怎么防并发 DMA？
7. BSP_UART 通道表有几个通道？分别是谁？
8. 全双工 DMA 的互斥策略？（忙标志 + 推迟 + 错误分类）
