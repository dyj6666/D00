# D00 源码研学 · 笔记 12 —— 信号发生器（UART/SPI/I2C 测试波形）

> 精读对象：`SystemServices/signal_gen.c`(505)
> 阶段 2 服务层第 8 批：产线自测与调试工具——三通道周期发帧

---

## 一、三通道总览

| 通道 | 硬件 | 实现方式 | 任务优先级 |
| --- | --- | --- | --- |
| UART | **USART6**（PC6=TX，AF8，覆盖 TIM8_CH1 PWM 复用） | **硬件 DMA**（DMA2_Stream6/CH5），传输期间任务睡眠 | Low |
| SPI | 软件 GPIO：SCK=PE5 / MOSI=PE6 / CS=PF6 | **软件 bit-bang**（模式0，MSB 先），无固定限速 | Low |
| I2C | 软件 GPIO：SCL=PE2 / SDA=PE3（开漏+上拉） | **软件 bit-bang**，DWT 周期计数延时（~5µs → 100kHz） | **High**（bit-bang 时序需避免被任务抢占） |

**共用任务模板**（sg_tx_task :178-209）：循环发帧 → `osThreadFlagsWait(SG_FLAG_STOP, 间隔ms)`——周期由事件标志等待控制，停止时立即退出。

## 二、关键实现

### UART 通道（sg_start_common :211-284）
- PC6 从 TIM8_CH1 PWM 复用**改为 USART6_TX**（GPIO 复用覆盖）
- DMA 发送 + 完成标志轮询（`while(!sg_uart_done) osDelay(1)`）——CPU 近零占用
- 停止（SG_UartStop :475-488）：`HAL_UART_DMAStop` + 置完成标志 + 停任务 + **20ms 等待任务退出当前帧**（<5ms @115200/64B，防与后续 Start 竞态）

### 软件 SPI（:163-176）
- 模式0（CPOL=0/CPHA=0）：SCK 低电平写 MOSI → SCK 拉高采样 → 循环 8 位
- CS 低有效，空闲高；GPIO 直驱极速（无时钟限制）

### 软件 I2C（:50-137）—— 完整 I2C 主发送器
- **DWT 周期计数延时**（:50-55）：`DWT->CYCCNT` 差 <840 周期 ≈ 5µs @168MHz → SCL 100kHz
- START/STOP/重复起始（Repeated START）时序齐全（:75-113）
- **可模拟 ACK/NACK**（:90-103）：ack_bit=1 模拟从机应答（SDA 拉低），0 → NACK——可仿真"从机不响应"
- 复杂演示帧（sg_i2c_tx_complex :126-137）：写3字节+ACK → **重复起始** → 读地址+ACK → 数据+NACK → STOP——完整覆盖 I2C 读流程
- SDA 开漏 + 上拉（:156-159）：读 ACK 可释放

### Hex 解析（sg_hex_value :286-292 + 各 Start 函数）
命令行十六进制字符串 → 字节数组（`sg_uart_hex "AA55..."`）。

## 三、设计亮点

1. **GPIO 复用覆盖**：PC6 从 TIM8_CH1 PWM 改为 USART6_TX（音频 I2S2_MCK 让位同理——引脚即资源，动态复用）
2. **DMA 睡眠发送**：UART 通道传输期间 CPU 近零占用
3. **事件标志周期控制**：停止即退出的任务模板，无忙等
4. **DWT 微秒延时**：bit-bang 时序精确到周期级，不依赖 SysTick
5. **ACK 模拟**：可仿真从机应答/不响应——产线测试利器
6. **Stop 竞态防护**：20ms 等待任务退出当前帧，防 Start/Stop 交错

## 四、待读清单（下一课）

- [x] signal_gen（本轮完成）
- [ ] `audio_svc.c`(330)：音频服务（I2S2+ES8388）
- [ ] `imu_svc.c`(279) + `imu_fusion.c`(77)：IMU 姿态
- [ ] `cam_link.c`(273)：摄像头链路（OpenART UART5）
- [ ] `touch_svc.c`(231)：触摸服务

## 五、自测题

1. 为什么 I2C 生成任务用 High 优先级而 UART/SPI 用 Low？（提示：bit-bang 时序）
2. SG_UartStop 为什么要等 20ms？不等会怎样？
3. 软件 I2C 的延时基准是什么？为什么不用 HAL_Delay？
4. ack_bit 参数模拟了什么？NACK 场景有什么用？
5. 复杂演示帧演示了 I2C 读的什么流程？与普通写帧的差异？
6. PC6 原本是什么功能？改成 USART6_TX 说明什么设计理念？
7. 周期发送如何实现"停止立即退出"？（提示：事件标志等待）
