# D00 源码研学 · 笔记 30 —— EEPROM 软 IIC 与低功耗 STOP 模式

> 精读对象：`BSP/bsp_eeprom.c`(306) · `bsp_power.c`(114)
> 阶段 3 BSP 第 4 批：参数存储介质 + 功耗管理

---

## 一、AT24C02 软 IIC 驱动（bsp_eeprom.c）—— 移植自正点原子

### 1.1 概况
- **软件模拟 IIC**：PB8=SCL / PB9=SDA，7 位地址 0x50（官方 GPIO 位操作，非硬件 I2C）
- 256B，8B 页写，任意字节就地改写（**EEPROM 非 Flash 仅清位**）
- 与 MPU6050（硬件 I2C1 / PB6-PB7）**完全独立，互不干扰**
- 内部互斥串行化；写周期等待 ≤5ms

### 1.2 关键实现
| 组件 | 实现 |
| --- | --- |
| 位时序 | **DWT 微秒延时**（168MHz 周期计数，~250kHz） |
| SDA 方向切换 | 直接改 GPIOB->MODER 位（sda_out/sda_in :62-71） |
| ACK 等待 | SDA 拉低检测 + 500µs 超时 |
| 读字节 | 最后字节 NACK，其余 ACK（iic_read_byte :138-166） |
| 随机读 | **重复起始**（iic_start 二次）切换读模式（:209） |
| **总线自恢复** | 释放 SDA + **9 个时钟脉冲** + STOP（eeprom_bus_recover :169-181）——从机钳位 SDA 时解困 |
| 页写拆分 | 页边界自动拆分（每页 ≤8B，:286-291） |
| 写周期 | 每页 osDelay(5ms) 等待内部写周期 |

### 1.3 互斥
- 所有 API 持互斥锁（50/100/200ms 超时）——多任务安全
- Probe：发写地址等 ACK（探测芯片存在）

## 二、⭐ 低功耗 STOP 模式（bsp_power.c）—— tickless 的安全边界

### 2.1 安全约束（头注释 :4-11）—— 最重要的设计
```
"IWDG 与 RTC 共用 LSI（未校准 17~47kHz）：RTC 1'秒'始终占 IWDG 预算约 25%
（两振荡器同源同比），因此睡眠上限取 2 RTC 秒（=50% IWDG 预算），
任何 LSI 频率下都安全"
```
→ **LSI 未校准（17~47kHz）**：RTC 秒与 IWDG 计数同源同比，比例恒定——**睡 2 RTC 秒 = 消耗 50% IWDG 预算**，任何频率下安全。

### 2.2 tickless 流程（BSP_Power_TicklessSleep :77-114）
```
FreeRTOS 空闲时调用（全任务阻塞）
  → 条件检查：power on + 空闲 ≥2s（<2s 不值得进 STOP）+ 非 eAbortSleep
  → 睡眠秒数 = 空闲时间/1000 向下取整（≤2s）
  → HAL_SuspendTick（STOP 期间内核时钟停）
  → 喂 IWDG（LSI 继续走）
  → 配 RTC 唤醒定时器（1Hz LSI 分频）→ EXTI 唤醒线
  → HAL_PWR_EnterSTOPMode（WFI，低功耗调节器）
  → 醒来：关唤醒线 → SystemClock_Config 重建时钟树（HAL 不自动恢复）
    → HAL_ResumeTick → vTaskStepTick(补休眠期间 tick)
```

### 2.3 其他
- `vApplicationIdleHook`（:32-35）：**纯 WFI**——任何中断（SysTick 1ms）唤醒，外设零影响，纯省功耗（不关 tickless 时也在用）
- tick 补偿按 RTC 秒数（与 RTC 同源）——系统 tick 与 RTC 保持一致，墙钟漂移由 SNTP 校准
- **默认关闭**（`power on` 才启用）：睡眠期间 CAN/ETH/UART 不接收
- 恢复时钟树：`SystemClock_Config`（CubeMX 配置函数重建 HSE→PLL→168MHz）

## 三、设计亮点

1. **LSI 同源比例恒定**：未校准振荡器下的 IWDG 预算精确计算——低功耗安全边界
2. **总线自恢复**：9 时钟脉冲解困从机钳位——软 IIC 鲁棒性
3. **重复起始读**：标准 IIC 随机读协议
4. **tick 补偿**：vTaskStepTick 补休眠时间——RTOS 时间线无缝
5. **纯 WFI 空闲钩子**：零成本省电（任何中断唤醒）
6. **默认关停**：低功耗与通信（CAN/ETH）互斥的工程取舍

## 四、待读清单（下一课）

- [x] bsp_eeprom / bsp_power（本轮完成）
- [ ] `bsp_es8388.c`(246) + `bsp_i2s.c`(161)：音频链
- [ ] `bsp_touch.c`(242) / `bsp_mpu6050.c`(187)：传感
- [ ] `bsp_system.c` / `bsp_watchdog.c` / `bsp_buzzer.c` / `bsp_gpio.c` / `bsp_flash.c` / `bsp_rtc.c` / `bsp_lcd.c` + LCD/

## 五、自测题

1. LSI 未校准时为什么睡眠上限是 2 RTC 秒？（IWDG 预算 50% 推导）
2. tickless 醒来后为什么必须重建时钟树？（HAL 不自动恢复）
3. vTaskStepTick 的作用？（补休眠期间的系统 tick）
4. 总线自恢复的 9 个时钟脉冲解决什么？（从机钳位 SDA）
5. 随机读为什么用重复起始？（写地址→读地址切换）
6. 为什么 `power on` 默认关闭？（睡眠期间 CAN/ETH 不接收）
7. 页写怎么拆分？（8B 页边界）
8. 纯 WFI 空闲钩子与 tickless 的区别？（前者仅省 CPU 空转，后者进 STOP）
