# D00 源码研学 · 笔记 31 —— 音频硬件链（ES8388 Codec + I2S2）

> 精读对象：`BSP/bsp_es8388.c`(281) · `bsp_i2s.c`(195)
> 阶段 3 BSP 第 5 批：DAC 播放链路——I2S2 主机 TX + DMA 双缓冲 + ES8388 配置

---

## 一、链路全景

```
audio_svc（波形合成/WAV）→ I2S2（SPI2 外设，主机 TX）
  → DMA1_Stream4/CH0（双缓冲循环 + TC 中断）→ PB12(WS)/PB13(SCK)/PC2(SD)/PC6(MCK)
  → ES8388 Codec（软 I2C PB8/PB9 配置）→ 喇叭/耳机
```

## 二、⭐ I2S2 驱动（bsp_i2s.c）

### 2.1 采样率公式与分频表（:7-9, 39-51）
```
Fs = I2SxCLK / [256 × (2×I2SDIV + ODD)]
I2SxCLK = (HSE/PLLM) × PLLI2SN / PLLI2SR  （HSE=8M, PLLM=8 → VCO 输入 1MHz）
```
**11 档分频表**（8k~192kHz）：每档预计算 `PLLI2SN/PLLI2SR/I2SDIV/ODD`——改采样率 = 查表重配 PLLI2S + I2SPR（:53-80）。

### 2.2 引脚
- WS=PB12(SCK=PB13) AF5；SDOUT=PC2 **AF6(I2S2ext)**；SDIN=PC3 AF5（录音预留）；**MCK=PC6（原 TIM8_CH1，已让位）**——引脚复用覆盖（与信号发生器 PC6 呼应）

### 2.3 ⭐ 两个血泪设计

**1. Play 前显式复位 DMA State/Lock**（:155-169）
```
"HAL_DMAEx_MultiBufferStart 成功后将 State=BUSY、Lock=LOCKED 且不释放；
若不清零，第二次 Play 会在 __HAL_LOCK/State 检查处直接 HAL_BUSY 返回，
DMA 静默不启动（重复播放/连续音效全失效）。"
```
→ 每次 Play 前：`State = READY + __HAL_UNLOCK`。

**2. 先使能 TC 中断再启动 DMA**（:163-166）
```
"先使能 TC 中断再启动 DMA：首个缓冲完成事件不被丢失
（原顺序 DMA 先跑、中断后开，首 TC 落在窗口外被 CLEAR 丢弃）"
```

### 2.4 双缓冲
- DMA_CIRCULAR + `HAL_DMAEx_MultiBufferStart`（buf0/buf1 交替）
- **TC 中断 = 半缓冲边界**（MultiBuffer 模式）→ 填充回调（audio_tx_cb）
- `BSP_I2S_GetCurrentBuf`（:181-184）：读 DMA CR.CT 位判断当前缓冲——填"未读"的那块

## 三、ES8388 Codec（bsp_es8388.c）—— 软 I2C 配置

### 3.1 概况
- **与 AT24C02 共享 PB8/PB9 软 I2C 总线**（注释 :7-9）：两驱动各自持锁，事务微秒级，低频访问竞争窗口可忽略；如需强互斥可抽公共总线锁
- 器件地址 0x10（7 位）；软 I2C 时序与 bsp_eeprom 同款（DWT 延时 ~250kHz）
- es_delay_us **带迭代上限防死循环**（:32-39，比 EEPROM 版更稳）

### 3.2 配置要点（Init :230-281）
| 寄存器 | 值 | 含义 |
| --- | --- | --- |
| 0x00 | 0x80→0x00 | **软复位** + 100ms 等待 |
| 0x02 | 0xF3→0xF0 | DAC/ADC 电源管理 |
| 0x03 | 0x09 | 麦克风偏置关闭 |
| 0x04 | 0x00 | DAC 电源（通道由 OutputCfg 开） |
| 0x09/0x0C/0x0D | 0x88/0x4C/0x02 | ADC PGA +24dB / 16bit / MCLK=256×fs |
| 0x17/0x18 | 0x18/0x02 | **DAC 16bit / MCLK=256×fs** |
| 0x1A/0x1B | 0x00 | DAC 数字音量最小（音量走 0x30/31） |
| 0x27/0x2A | 0xB8 | L/R 混频器 |
| 0x30/0x31 | 音量 | 喇叭 L/R（0~33） |

### 3.3 接口
- AddaCfg(dacen, adcen)：DAC/ADC 使能
- OutputCfg(o1en, o2en)：输出通道（喇叭/耳机）
- I2sCfg(fmt, len)：Philips + 16bit
- SpkVolSet/HpVolSet：音量（钳 33）

## 四、设计亮点

1. **查表改采样率**：PLLI2S 重配 + I2SPR 分频——任意 WAV 采样率即切即用
2. **双缓冲无缝**：TC 半缓冲边界填充——播放不中断
3. **HAL 状态机陷阱预判**：Play 前显式复位（HAL_BUSY 静默失败教训）
4. **中断时序**：先开中断再启 DMA（首 TC 不丢）
5. **总线共享声明**：与 EEPROM 共用软 I2C 的互斥策略说明（各自锁 + 微秒事务）

## 五、待读清单（下一课）

- [x] es8388 / i2s（本轮完成）
- [ ] `bsp_touch.c`(242) / `bsp_mpu6050.c`(187)：传感输入
- [ ] `bsp_system.c` / `bsp_watchdog.c` / `bsp_buzzer.c` / `bsp_gpio.c` / `bsp_flash.c` / `bsp_rtc.c`
- [ ] `bsp_lcd.c`(444) + `LCD/lcd.c`(1195) + `LCD/lcd_ex.c`(1568)

## 六、自测题

1. I2S 采样率公式？PLLI2S 的作用？（VCO 输入 1MHz 查表）
2. 为什么 Play 前必须复位 DMA State/Lock？（HAL_BUSY 静默失败）
3. 为什么先开 TC 中断再启 DMA？（首 TC 事件丢失）
4. 双缓冲的 TC 中断代表什么？（半缓冲边界）
5. GetCurrentBuf 怎么判断？（CR.CT 位）
6. ES8388 与 EEPROM 共享总线的互斥策略？（各自锁 + 微秒事务）
7. MCK=PC6 原本是什么？（TIM8_CH1 让位）
8. DAC 音量为什么走 0x30/31 而 0x1A/1B 置 0？（音量分级控制）
