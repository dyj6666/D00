# D00 源码研学 · 笔记 10 —— 用户存储（EEPROM 日志）与系统监控

> 精读对象：`SystemServices/usr_store.c`(403) · `sysmon.c`(256)
> 阶段 2 服务层第 6 批：参数持久化（EEPROM 日志式） + 运行时健康监控

---

## 一、⭐ 用户存储（usr_store.c）—— EEPROM 上的日志式键值库

**介质**：AT24C02 类 EEPROM（软件 IIC PB8/PB9 @0x50，`USR_EEPROM_SIZE`），**EEPROM 可字节擦写、寿命 100 万次**——所以用"日志追加"而非"原地更新"（Flash 才需要擦除）。

### 1.1 记录格式
```
[magic 0xA5][key 1B][len 1B][crc16 2B][data lenB]     ← 头 5B + 数据
len=0xFF = 墓碑（删除标记）
CRC16 覆盖 magic+key+len+data（墓碑不含 data）
```

### 1.2 核心机制
| 机制 | 实现 |
| --- | --- |
| **追加写**（UsrStore_Set :219-262） | 每次 Set 在日志尾部追加新记录（不覆盖旧值）——磨损均衡 + 掉电安全 |
| **读最新**（rec_find_latest :98-125） | 单遍扫描取 key 最后一条有效记录；墓碑 → -2（已删） |
| **删除 = 写墓碑**（UsrStore_Erase :264-297） | 追加 len=0xFF 记录，不物理擦除 |
| **空间耗尽 → compact**（store_compact :128-188） | 扫描收集各 key 最新值 → 整片写 0xFF → 重写紧凑日志 |
| **损坏自动重格式化**（UsrStore_Init :375-399） | 首字节非 0xFF/0xA5（垃圾字节）→ 判损坏 → 整片擦除重来 |

### 1.3 设计亮点
1. **日志式 = 掉电安全**：追加写中途断电只损失新记录，旧数据完好（无 in-place 破坏）
2. **compact 静态暂存**（:29-31）：3×256B 静态数组（互斥保护），无堆分配、无任务栈压力
3. **0xFF 既是"空闲"又是"擦除后"**：`rec_scan` 返回 0=干净结束 / 0xFFFF=垃圾字节损坏（:57-58）
4. **单记录超限降级**：append 放不下 → compact → 还放不下 → 失败（:232-240）
5. **key 位图统计**（UsrStore_Count :314-347）：32B 位图统计活跃 key 数，无堆

> ⚙️ 对比：ext_store（外部 Flash）用"双份+提交点"（擦写成本高），usr_store（EEPROM）用"日志+墓碑"（可字节写）——**按介质特性选持久化策略**。

## 二、⭐ 系统监控（sysmon.c）—— 喂狗放 SysTick 的工业级决策

### 2.1 硬件看门狗喂狗在中断上下文（:25-34）——最重要的设计
```c
void vApplicationTickHook(void) {
    ERR_TickMs = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;  // 崩溃 uptime 快照
    if (++tick_cnt >= WDOG_FEED_PERIOD_MS) {   // 1kHz tick → 1s
        tick_cnt = 0;
        BSP_Watchdog_Refresh();   // ★ 中断上下文喂狗
    }
}
```
**注释明示的工业级原则**（:21-24）：硬件看门狗**绝不可依赖低优先级任务**——事件风暴/高优先级任务长时间占用 CPU 会饿死 Tmr Svc，导致 IWDG 误复位。SysTick 钩子喂狗，**任何任务调度风暴都不影响喂狗**。

### 2.2 监控项注册表（:39-58）
可扩展：模块在各自 Init 调用 `SysMon_RegisterItem` 注册采集函数——**避免 sysmon 向上依赖应用层**（依赖方向反转）。核心项：Tasks / CPU Usage / Heap / Watchdog / Reset Reason / Event Bus / DataLink / Last Crash。

### 2.3 CPU 占用率两拍差分（:121-178）
- **DWT 周期计数器 32 位 @168MHz ≈ 25.6s 回绕**：直接对"启动以来累加值"求百分比是垃圾数据
- 改为**相邻两次快照差分**：窗口远小于回绕周期，数值稳定
- **两拍均非阻塞**：第一拍只采样基准立即返回（打印 "sampling, run sysmon again"）
- **关键约束**（:119-120）：sysmon 在事件总线任务（Realtime 优先级）内执行，**绝不能在 Realtime 任务里 vTaskDelay(1000)**（曾实测阻塞 1s 拖垮全部消息）

### 2.4 输出节流（:219-222）
监控项间 `vTaskDelay(25ms)`：UART@115200 排水 ~11.5KB/s，不加延时整段输出会撑满 2KB LOG 流缓冲导致中段截断（sysmon 曾只显示首行）。

### 2.5 其他
- **ERR_Init + ERR_ReportLastCrash**（:231-232）：启动时复现上次崩溃（与 BOOT 呼应）
- **DEBUG 模式**（:234-242）：APP_DEBUG_MODE=1 时不启动 WDOG（gdb 断点调试用）
- **Reset Reason**：BSP_GetResetReason 读 RCC->CSR——区分 IWDG/WWDG/上电/引脚/软复位（诊断"谁复位了我"）

## 三、设计亮点汇总（本轮两文件）

1. **看门狗喂狗进中断**：SysTick 钩子喂 IWDG——任务风暴免疫（工业级正确姿势）
2. **介质适配持久化**：EEPROM 日志式 vs Flash 双份式——按介质选策略
3. **墓碑删除**：删除零成本（一个字节标记），compact 时才物理清理
4. **两拍差分 CPU 统计**：DWT 回绕免疫 + Realtime 任务内不阻塞
5. **依赖方向反转**：监控项注册表让应用层反向注册，服务层不向上依赖
6. **输出节流**：25ms 间隔防 2KB 流缓冲撑爆

## 四、待读清单（下一课）

- [ ] `la_sample.c`(359) + `la_buffer.c` + `la_trigger.c`：逻辑分析仪（DMA 采样 + 触发）
- [ ] `signal_gen.c`(453)：信号发生器（UART/SPI/I2C）
- [ ] `audio_svc.c`(330) + `wav_data.c`：音频服务
- [ ] `imu_svc.c`(279) + `imu_fusion.c`：IMU 姿态
- [ ] `cam_link.c`(273)：摄像头链路
- [ ] `touch_svc.c`(231)：触摸
- [ ] 网络服务批：eth_app(706) / tcp_svc / icmp_svc / dns_svc / sntp_svc / mqtt_svc / http_svc

## 五、自测题

1. 为什么 usr_store 用"追加写"而 ext_store 用"双份覆盖"？（提示：介质差异）
2. 删除一个 key 会发生什么？立即物理擦除吗？什么时候才清理？
3. compact 的触发条件？compact 期间掉电会怎样？
4. rec_scan 返回 0 / 0xFFFF / 正数分别代表什么？为什么 0xFF 不是损坏？
5. 喂狗为什么必须在 SysTick 钩子而不是 Tmr Svc 或任务？任务风暴场景推演一下。
6. CPU 统计为什么用两拍差分？直接累计会怎样？（提示：DWT 回绕）
7. sysmon 为什么不能在 Realtime 任务里 sleep 1s？实测发生过什么？
8. 监控项之间的 25ms 延时解决什么问题？
