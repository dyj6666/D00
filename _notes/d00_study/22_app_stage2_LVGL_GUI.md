# D00 源码研学 · 笔记 22 —— LVGL GUI 体系

> 精读对象：`Application/gui_app.c`(404) · `gui_pages.c`(2020+) · `gui_theme.c`(128)
> 阶段 2 应用层第 7 批：240×320 深色主题七页面 GUI（LVGL v8.3.5）

---

## 一、架构总览

```
GuiApp_Init（模块 prio 55）
  → BSP_LCD_Init → lv_init（LVGL 堆在外部 SRAM）→ LvPort_Init（显示+触摸端口）
  → GuiPages_Init（构建 7 页面 + 加载主页）→ 订阅按键事件 → 建 GuiApp 任务（8KB 栈）

GuiApp 任务（5ms 周期）：
  lv_timer_handler()（驱动全部动画/事件）→ DWT 测渲染耗时
  → 消费按键/挥手导航标志 → 250ms 三相轮转刷新 → vTaskDelay(5ms)
```

## 二、⭐ 七页面（gui_pages.c）

| 页面 | 内容 |
| --- | --- |
| 主页 HOME | 摘要条（CPU/HEAP/UP）+ 固件版本 + 8 张状态卡（三态色点：OK/WARN/ERR） |
| 网络 NET | 链路状态/网关/MAC/DHCP + **吞吐曲线**（1s 一点滚动，60 点=1 分钟窗口） |
| 系统 SYS | CPU 弧表（动画）+ 堆进度条 + 状态行 |
| SRAM | 内存池占用条 + 统计 + 自检/FSMC/基准行 |
| 音频 AUDIO | 频率对数滑条 + 音量滑条 + 预设/播放/WAV 按钮 |
| 摄像头 CAM | 手部坐标/手势/挥手统计 + 跟踪点 |
| 云台 GIMBAL | **双轴弧形仪表（pan/tilt）+ PID 参数卡 + FOV 网格 + 通道切换 + 偏差曲线** |

**导航**：底部导航栏（HOME/NET/SYS…）带切换动画；**页面对象常驻零重建**（切页不销毁，刷新只更新控件）。

## 三、⭐ 关键设计（gui_app.c）

### 1. 8KB 任务栈血泪（:34-37）
`gui bench` 全量渲染（lv_refr_now 深调用）峰值超 4KB（实测骨架 1.5KB，全量渲染更高）；**4KB 时 bench 栈溢出踩堆 → 对象指针损坏 → lv_obj_get_parent HardFault**（ENGINEERING_LOG 13.2）。

### 2. 优先级 Normal(24) 血泪（:393-397）
曾试提至 32 与 TouchSvc/tcpip 同级——**同级时间片轮转导致渲染被触摸采样任务每 10ms 打断、碎片化，实测触摸"卡到不起作用"**。24 时触摸采样任务(32)可抢占 GUI，但每次仅几十 µs，可接受。

### 3. 事件总线回调只置标志（:48-66）
LVGL API 非线程安全——KEY 事件回调（eventBusTask 上下文，优先级高于 GUI）**只置标志**，实际切换由 GUI 任务消费执行。

### 4. 挥手翻页防抖（:96-105）
500ms 内仅响应一次——OpenART AI 挥手模型静态场景可能误报，连续误报导致频繁翻页动画；**高频动画是 FSMC 偶发挂起（ENGINEERING_LOG 10.25）的放大因素**。

### 5. 250ms 三相轮转刷新（:111-116）
采集/曲线/文本分片，**彻底错峰**——避免所有控件同帧爆发重绘；数据粒度 250ms 使数值更新更平滑。

### 6. GUI bench（`gui bench` 命令）
- 内存带宽：EXT-SRAM/SRAM128/CCM/FLASH 读 + 外部 SRAM 写（DWT 周期计时）
- LVGL 场景：fill 全屏 / 12 色带 / UI 阴影开关对比 / 60×60 动画——**lv_refr_now 直测整帧耗时 + flush MPix/s**
- **基准跑在独立临时屏**（s_bench_scr）：场景 clean/重建不破坏常驻页面对象（悬垂指针崩溃教训 13.2）

### 7. 主题工厂（gui_theme.c）
卡片/状态点/色条/标签/标题栏 + 三态颜色（OK/WARN/ERR）——**所有界面组件统一样式来源**，`remove_style_all` + 全自定义（不依赖默认主题）。

## 四、设计亮点

1. **页面常驻零重建**：切页动画不销毁对象——刷新零延迟
2. **渲染任务化**：lv_timer_handler 5ms 驱动，无忙轮询（空跑 ~20µs，CPU 增量 <1%）
3. **线程安全隔离**：回调置标志 / 任务消费——LVGL 单线程约束的正确姿势
4. **性能可观测**：DWT 实测渲染耗时 + bench 全套（内存带宽/帧耗时/MPix/s）
5. **轮转刷新错峰**：250ms 三相分片，避免同帧爆发重绘
6. **手势翻页防抖**：误报放大因素的工程防护

## 五、待读清单（下一课——Application 收尾）

- [x] gui_app / gui_pages（结构）/ gui_theme（本轮完成）
- [ ] `data_agent.c` / `buzzer_app.c` / `key_app.c` / `led_app.c` / `cmd_can.c` / `cmd_catalog.c` 剩余
- [ ] `ctrl/`：pid / filter / kalman / test_ctrl（控制算法库）
- [ ] 之后转 BSP 层（LCD/W25Q/UART/CAN/ES8388...）

## 六、自测题

1. GUI 任务为什么 8KB 栈？4KB 时发生过什么？（栈溢出→对象损坏→HardFault）
2. GUI 优先级为什么保持 Normal(24)？提到 32 会发生什么？（触摸卡死）
3. 事件总线回调为什么只置标志不直接调 LVGL？（非线程安全）
4. 挥手翻页为什么防抖 500ms？（误报→频繁动画→FSMC 挂起放大）
5. 250ms 三相轮转刷新的目的？（错峰避免同帧爆发重绘）
6. bench 为什么跑在独立临时屏？（防 clean 破坏常驻页面）
7. 主题工厂的设计意图？（统一样式来源）
8. lv_timer_handler 5ms 周期空跑多少 CPU？（~20µs，<1%）
