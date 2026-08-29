# D00 源码研学 · 笔记 32 —— MPU6050 驱动与系统服务

> 精读对象：`BSP/bsp_mpu6050.c`(210) · `bsp_system.c`(59)
> 阶段 3 BSP 第 6 批：IMU 底层 + 系统统一入口

---

## 一、⭐ MPU6050 驱动（bsp_mpu6050.c）—— IT 模式 + 三级自恢复

### 1.1 传输策略（reg_op :51-102）
```
BSP_I2C1_Lock → ★ 强制修正 I2C1 CCR/TRISE 寄存器（400kHz 快速模式）
  → IT 模式优先（HAL_I2C_Mem_Write/Read_IT + 完成信号量）——CPU 占用从 ~8% 降到 ~1%
  → 失败回退：总线自恢复（9 时钟）+ 阻塞模式重试一次
```

**⭐ CCR/TRISE 兜底修正**（:55-61）——血泪注释：
```
"强制 I2C1 400kHz 快速模式时序（HAL_I2C_Init 在部分构建产物中 CCR/TRISE
配置异常——实测 CCR=0x0D/0x8023 均非 400kHz 值，导致总线时钟异常、传输
BERR。此处每次传输前兜底修正：
  CCR   = 0x8035（Fm，42MHz/(2×400kHz)≈53）
  TRISE = 43（42MHz×1µs 上升时间 + 1）"
```
→ **每次传输前直接写寄存器兜底**——不信任 HAL 初始化结果（构建产物差异导致的隐蔽 bug）。

### 1.2 中断完成信号（:35-48）
- 三个 HAL 回调（MemRx/MemTx/Error）→ `i2c_it_done` → 信号量给（ISR 安全）
- 传输前清残留信号（`xSemaphoreTake(0)`）

### 1.3 初始化（:122-155）
```
BSP_I2C1_Init → I2C1 EV/ER 中断（优先级 7）→ 探测（0x68/0x69 双地址 + 总线释放重试）
→ 复位器件（PWR_MGMT_1=0x80）→ 100ms
→ 退出睡眠 + PLL X 轴陀螺时钟源（最高精度）
→ 采样率 1kHz/(1+4)=200Hz + DLPF 98Hz（硬件抗混叠）
→ 量程最高分辨率：±250dps（131 LSB/dps）/ ±2g（16384 LSB/g）
```

### 1.4 校准（Calibrate :172-205）
- 陀螺零偏：N 样本平均（int64 累加防溢出）
- **加速度偏移条件应用**：仅当校准姿态近似水平（|a|²∈[0.90,1.10]）才应用，否则清零——防非水平校准污染

## 二、系统服务（bsp_system.c，59 行）

| 接口 | 实现 | 血泪 |
| --- | --- | --- |
| BSP_SystemReset | NVIC_SystemReset | — |
| **BSP_DelayMs** | **自旋**（ms×16800 循环） | ★ HAL_Delay 依赖 tick——**tick 异常（上下电后中断瘫痪）时无限等待，启动早期卡死 → LCD 白屏** |
| BSP_GetTick | HAL_GetTick | — |
| BSP_DWT_Enable / GetCycleCount | DWT 周期计数 | 全项目微秒计时基准 |
| **BSP_GetResetReason** | RCC->CSR 标志解析 + RMVF 清除 | IWDG/WWDG/上电/引脚/软复位——"谁复位了我"诊断 |

## 三、设计亮点

1. **IT 模式省 CPU**：任务休眠等待完成信号——8% → 1% 占用
2. **三级自恢复**：总线释放（9 时钟）→ 地址切换（0x68→0x69）→ 阻塞重试
3. **不信任 HAL**：CCR/TRISE 每次兜底修正（构建产物差异 bug）
4. **条件校准**：加速度偏移仅水平姿态应用
5. **自旋延时**：启动早期 tick 异常免疫（白屏事故教训）

## 四、待读清单（下一课）

- [x] mpu6050 / system（本轮完成）
- [ ] `bsp_touch.c`(242) / `bsp_watchdog.c` / `bsp_buzzer.c` / `bsp_gpio.c` / `bsp_flash.c` / `bsp_rtc.c`
- [ ] `bsp_lcd.c`(444) + `LCD/lcd.c`(1195) + `LCD/lcd_ex.c`(1568)

## 五、自测题

1. CCR=0x8035 / TRISE=43 怎么算的？（42MHz/(2×400k)≈53；42M×1µs+1）
2. 为什么每次传输前兜底修正 CCR？（HAL 初始化配置异常）
3. IT 模式比阻塞省多少 CPU？（8%→1%）
4. 探测失败的三级恢复顺序？（总线释放→换地址→再试）
5. 加速度偏移为什么条件应用？（非水平校准污染）
6. BSP_DelayMs 为什么自旋？（tick 异常时 HAL_Delay 无限等待→白屏）
7. 复位原因怎么读？（RCC->CSR + RMVF 清除）
8. MPU6050 采样率怎么配的？（1kHz/(1+4)=200Hz）
