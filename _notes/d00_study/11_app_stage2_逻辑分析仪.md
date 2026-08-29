# D00 源码研学 · 笔记 11 —— 逻辑分析仪（LA）三件套

> 精读对象：`la_sample.c`(405) · `la_buffer.c`(97) · `la_trigger.c`(91)
> 阶段 2 服务层第 7 批：板载 4 通道逻辑分析仪——两种采样模式 + 触发门控

---

## 一、整体架构

```
┌────────────────────────────────────────────────────────────────┐
│ 模式 A：时间戳模式（EXTI 中断，事件驱动）                        │
│   通道电平跳变 → EXTI(9-5/15-10) → HAL_GPIO_EXTI_Callback       │
│     → 记 48bit 时间戳（TIM2 32bit + 溢出扩展）+ 8bit 通道状态     │
│     → 预触发环形缓冲（外部 SRAM 6KB，1024 点）→ 触发 → 主缓冲     │
│ 模式 B：DMA 流模式（硬件定时采样）                               │
│   TIM1 更新事件 → DMA2_Stream5 → GPIO IDR 整字 → 外部 SRAM 环形   │
│   32768 点，采样率 PSC/ARR 可调，无触发（纯流）                   │
└────────────────────────────────────────────────────────────────┘
   数据导出：HOSTLINK CMD_LA_DUMP（60 样本/帧背压发送）→ 上位机分析
```

## 二、模式 A：时间戳模式（EXTI 中断驱动）

**采样点结构**（6B）：`timestamp_lo(16bit) + timestamp_hi(16bit) + states(16bit)`
- **时间戳**：TIM2 32bit 计数器（168MHz/分频）+ `ts_overflow` 扩展 → 48bit 时间（LA_Sample_GetTimestamp :174-183）
- **状态**：`LA_Sample_GetChannelStates`（:185-196）读 `LA_GPIO_PORT->IDR` 打包成位图（数据位 i = 通道 i，未用通道恒 0）

**HAL_GPIO_EXTI_Callback**（:216-279）核心逻辑：
```
跳变到来
 ├─ 未触发：写入预触发环形缓冲（PRE_TRIGGER_DEPTH=1024 点，外部 SRAM）
 │    └─ 触发检测：边沿匹配（上升/下降/任意）+ 条件通道电平匹配（如 I2C START）
 │         → trigger_fired=1，post_trigger_count=0
 └─ 已触发：直接写主缓冲（外部 SRAM 512KB 区）
      └─ post_samples 满足 → capture_done=1 → 关 EXTI（自动停采）
停止时：预触发缓冲按时间顺序补到主缓冲前部（LA_Sample_Stop :159-168）
```

**触发条件**（la_trigger.c）：
```
边沿：RISING / FALLING / ANY（对比上一状态）
条件：cond_channel 电平 == cond_level（可选，如 I2C START = SDA 下降沿 + SCL 高）
触发一次后自动解除武装（SetTriggered :87-90，防重复触发）
```

## 三、模式 B：DMA 流模式（TIM1 + DMA2 硬件采样）

**引擎**：TIM1 更新事件 → DMA2_Stream5(Ch6) 把 `LA_GPIO_PORT->IDR` 整字搬入环形缓冲。
**关键硬件事实**（注释 :46-52）：
1. **DMA1 无法访问 AHB1（GPIO）**——所以不能用 TIM3+DMA1，必须 TIM1+DMA2（DMA2 才能访问 AHB1）
2. **单环形缓冲**（32768 点）非 ping-pong——半/全传输回调仅计数，数据须停采后统一导出
3. **采样率**：TIM1 在 APB2(168MHz)，PSC/ARR 组合精确设定（:298-314 在 16bit 范围内暴力搜索最接近分频）

**流程**：`LA_Sample_Start_DMA(rate)` → 算 PSC/ARR → `HAL_DMA_Start_IT` → 使能 TIM1 DMA → 启动。
**停止**：先取计数（`dma_transfer_count * size + (size - DMA 剩余)` :381-389）→ 停 DMA → Abort。
**读取**（:391-405）：原始 IDR 字 → **归一化打包**成通道位图（与模式 A 数据格式一致，上位机无需区分）。

## 四、外部 SRAM 自检（la_buffer.c:27-52）——血泪教训

```
32KB 抽查（原 512KB 全量！）：
  · 全量写读的"故障检测覆盖率"收益极低（32KB 即可暴露芯片/FSMC 级故障）
  · 实测 SRAM 响应异常时全量自检可拖长到分钟级、占满 CPU（GUI 刷新被压制）
  · 写循环+读循环都带 500ms 超时（正常 <10ms）——异常时秒级结束，不阻塞启动
  · 若总线彻底停摆（CPU 卡在 ldrh），由 IWDG 兜底复位
```
**启动防爬行**：任何模块初始化都不能无超时地长时间占 CPU——这是本项目的通用纪律（ENGINEERING_LOG 13.5）。

## 五、预触发缓冲迁移史（la_sample.c:30-34）

预触发缓冲**原放 CCM（6KB，CCM 占用 98.8% 紧张）**→ 优化后移至外部 SRAM 空闲区（MEM_LA_PRETRIG_BASE）。LA 为调试工具，时间戳模式是事件触发写入（非连续流），FSMC 写外部 SRAM 延迟可接受；**CCM 腾出 6KB 供 FreeRTOS 堆扩展**。

## 六、设计亮点

1. **双模式互补**：时间戳模式=事件精确（边沿触发+条件）；DMA 模式=连续流（高采样率）——同一导出格式
2. **48bit 时间戳**：32bit TIM2 + 溢出计数扩展，长时捕获不丢时间
3. **条件触发**：边沿+电平组合（I2C START 级别场景）
4. **数据归一化**：两种模式导出相同位图格式，上位机协议统一
5. **自检防拖死**：32KB 抽查 + 500ms 超时——启动绝不因 SRAM 故障爬行
6. **硬件选型纪律**：DMA2 才能访问 AHB1（GPIO）的硬件事实写进注释

## 七、待读清单（下一课）

- [x] la_sample / la_buffer / la_trigger（本轮完成）
- [ ] `signal_gen.c`(453)：信号发生器（UART/SPI/I2C 三通道）
- [ ] `audio_svc.c`(330) + `wav_data.c`：音频服务
- [ ] `imu_svc.c`(279) + `imu_fusion.c`：IMU 姿态
- [ ] `cam_link.c`(273)：摄像头链路
- [ ] `touch_svc.c`(231)：触摸服务

## 八、自测题

1. 为什么 DMA 流模式不能用 TIM3+DMA1？（提示：总线访问权限）
2. 时间戳模式的 48bit 时间戳怎么来的？TIM2 溢出时会发生什么？
3. 触发检测的"条件通道"解决什么场景？（提示：I2C START）
4. 为什么触发一次后要解除武装？
5. 停止时预触发缓冲为什么要"按时间顺序补到主缓冲前部"？不补会怎样？
6. SRAM 自检为什么从 512KB 全量改成 32KB 抽查？两个循环都带超时的原因？
7. DMA 缓冲为什么是"单环形"而不是 ping-pong？注释里为什么强调命名？
8. LA_Sample_ReadDMABuffer 的归一化做了什么？两种模式数据格式一致有什么好处？
