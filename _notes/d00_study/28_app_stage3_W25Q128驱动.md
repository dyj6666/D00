# D00 源码研学 · 笔记 28 —— W25Q128 驱动（APP vs BOOT 双版本对照）

> 精读对象：`BSP/bsp_w25q128.c`(570)
> 阶段 3 BSP 第 2 批：**同一颗 W25Q128 的 APP 侧完整驱动**——与 BOOT esp_flash.c(287) 对照学习

---

## 一、传输策略三层（头注释 :4-10）

```
短命令（<16B）：寄存器级阻塞全双工，DWT 微秒超时
数据段读：Fast Read(0x0B) + DMA2 双通道（RX=Stream0/TX=Stream3，CH3），
          512B dummy 缓冲循环复用，42MHz 满速零 CPU 干预
数据段写：页编程(0x02) 阻塞满速，跨页自动拆分
忙等待：轮询 SR1/WIP，自旋 2ms 节流 + 定期喂狗
```

## 二、⭐ 与 BOOT esp_flash 的对照

| 维度 | BOOT esp_flash（287 行） | APP bsp_w25q128（570 行） |
| --- | --- | --- |
| 传输 | 寄存器级逐字节（TXE/RXNE） | 短命令同左；**数据段 DMA2 双通道**（零 CPU） |
| 读 | 0x03 普通读 | **0x0B Fast Read**（+1 dummy，更高带宽） |
| 超时 | HAL_GetTick 毫秒 | **DWT 微秒**（不依赖 tick，tick 异常不误判） |
| 忙等 | 轮询 + 喂狗 | 自旋 2ms 节流（**不依赖 HAL tick**）+ 每 ~128ms 喂狗 |
| 互斥 | 无 OS 不需要 | **全局忙锁**（关中断 test-and-set，非阻塞） |
| 分块 | 页写自动跨页 | 同左 + DMA 512B 块循环（CS 全程低连续读） |
| 诊断 | 简洁 | **DMA 超时全寄存器转储**（LISR/SxCR/NDTR/M0AR/SPI CR/RCC） |

## 三、⭐ 三个关键设计（全是血泪）

### 1. CCM 陷阱（:59-62）
```
"FreeRTOS heap 位于 CCM RAM(0x10000000)，任务栈/堆缓冲 DMA 无法访问
（总线错误 TEIF0）。所有读操作先 DMA 入本缓冲，再拷贝到调用方缓冲。"
```
→ `s_dma_rx` 放普通 SRAM（aligned 4），**DMA 先入内部缓冲再 memcpy 到用户缓冲**（支持 CCM/任意地址）。与 cam_link 的教训同源（DMA 不能访问 CCM）。

### 2. 自管理 DMA（寄存器级，:153-233）
- **不依赖 HAL SPI DMA 状态机**——直接操作 DMA2 Stream0/3 寄存器
- **TX 多 1 个 dummy 时钟**（NDTR = len+1）：SPI 全双工，RX 末字节需要额外时钟才移出（:171）
- **先清标志再启动**；等 TCIF0 完成；超时 = 传输时间×2 + 下限 5ms（正常块 ~104µs）
- **超时全寄存器转储诊断**（:200-216）：LISR/S0CR/S0NDTR/S0M0AR/S3CR/S3NDTR/S3M0AR/SPI CR1/CR2/SR + RCC 使能位——定位 DMA 挂死根因的取证手段

### 3. 忙等待自旋（:269-289）
- **自旋 2ms 不依赖 HAL tick**——注释："tick 异常时 HAL_Delay 无限等待"
- 每 ~128ms 喂狗（防长擦除复位，擦除最大 300s 芯片擦除）

### 4. 忙锁（:70-87）
- 关中断 test-and-set——**非阻塞互斥**（长操作期间其他调用直接返回 BUSY）
- 内部函数假定已持锁（分层清晰）

### 5. JTAG 释放注释（:295-297）
- F407 无 SYSCFG_CFGR（F1/F429 才有）；PB3/PB4 默认 AF0(SWJ)，HAL_SPI_Init 配 AF5(SPI1) 后 JTAG 自动让位，SW-DP(PA13/14) 不受影响，**DAP 仍可用**

## 四、设计亮点

1. **DMA 零 CPU 读**：512B 块循环 + CS 全程低（连续读模式）——大块导出（LA_DUMP/OTA）性能关键
2. **DWT 微秒超时**：不依赖 HAL tick——tick 异常免疫
3. **CCM 防护**：DMA 先入 SRAM 缓冲再拷贝——任意调用方缓冲安全
4. **超时取证**：DMA 挂死时全寄存器转储——可诊断可复现
5. **忙锁非阻塞**：长操作互斥，调用方感知 BUSY

## 五、待读清单（下一课）

- [x] bsp_w25q128（本轮完成）
- [ ] `bsp_can.c`(313) / `bsp_eeprom.c`(281) / `bsp_sram.c`(226)
- [ ] `bsp_es8388.c`(246) / `bsp_i2s.c`(161)：音频链
- [ ] `bsp_touch.c`(242) / `bsp_mpu6050.c`(187)：传感输入
- [ ] `bsp_power.c` / `bsp_system.c` / `bsp_watchdog.c` / `bsp_buzzer.c` / `bsp_gpio.c` / `bsp_flash.c` / `bsp_rtc.c` / `bsp_lcd.c` + LCD/

## 六、自测题

1. DMA 为什么不能直接读进 CCM 缓冲？（TEIF0 总线错误）怎么解决？
2. TX DMA 为什么 NDTR = len+1？（RX 末字节需要额外时钟）
3. 忙等为什么自旋而不 HAL_Delay？（tick 异常时无限等待）
4. 忙锁怎么实现非阻塞互斥？（关中断 test-and-set）
5. DMA 超时转储哪些寄存器？为什么这么全？（取证）
6. APP 驱动比 BOOT 多了什么能力？（DMA 读/诊断/忙锁）
7. Fast Read 与普通读的区别？（+1 dummy 周期）
8. JTAG 让位的原理？（PB3/4 AF0→AF5，SW-DP 不受影响）
