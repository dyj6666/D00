# D00 源码研学 · 笔记 23 —— 交互反馈：蜂鸣器 / 按键 / LED

> 精读对象：`Application/buzzer_app.c`(175) · `key_app.c`(47) · `led_app.c`(151)
> 阶段 2 应用层第 8 批：人机交互三件套（声音/输入/指示）

---

## 一、⭐ 蜂鸣器（buzzer_app.c）—— 双模式时序状态机

### 1.1 非阻塞序列模式（Tmr Svc 定时器驱动）
```
Buzzer_PlaySequence([on0,gap0,on1,gap1,...], n)：
  → xTimerStop（取消进行中序列）
  → 拷贝 2n 项（★ 血泪：若只拷 n 项，后续 gap 读到静态零值，
       触发 xTimerChangePeriod(0) 的 FreeRTOS 断言 timers.c:836）
  → Buzzer_On + xTimerChangePeriod(seq[0])

buzzer_timer_cb（响/停交替）：
  phase=0 响结束 → Off → 若有下一段 → phase=1 → ChangePeriod(gap)
  phase=1 间隙结束 → idx++ → On → phase=0 → ChangePeriod(on)
```

### 1.2 阻塞模式（OTA 专用，buzzer_ota_block :112-125）
- **不依赖 Tmr Svc 调度**（OTA 流程中 Tmr Svc 可能被高优先级任务饿死）
- 阻塞总时长 ≤1s；BSP_DelayMs 驱动——从任务上下文调用，期间暂停该任务（可接受）

### 1.3 OTA 旋律语汇（有源蜂鸣器：节奏即音高表达）
| 事件 | 旋律 | 含义 |
| --- | --- | --- |
| Buzzer_OtaStart | 滴-滴-嘟（两短一长） | 升级开始/下载就绪 |
| Buzzer_OtaDownloadDone | 滴-滴（双短） | 下载完成，随后触发 BOOT 切换 |
| Buzzer_OtaSuccess | 滴-滴-滴-嘟（三短一长） | **新固件启动确认成功** |
| Buzzer_OtaFail | 滴-滴-滴（三短） | 升级失败（RECOVERY 也用它） |

## 二、按键（key_app.c，47 行）—— 定时器扫描 + 软件消抖

```
10ms 定时器回调：
  按下：连续 5 次采样视为有效（消抖）；累计时长 ≥1s → MSG_KEY_LONG（重复触发）
  松开：有效按下且 10ms ≤ 时长 < 1s → MSG_KEY_SHORT
事件经事件总线广播（GUI 翻页 / 蜂鸣反馈 / LED 多订阅，广播语义互不冲突）
```

## 三、⭐ LED（led_app.c）—— 状态机表驱动

```
state_table[LED_STATE_COUNT] = { state_off, state_on, state_slow_blink, state_fast_blink }

led_switch_state(new)：切状态 → 同步上位机变量 → 调用新状态函数（msg=NULL 表示进入动作）
led_msg_handler（统一入口）：
  · MSG_CMD_LED（led 命令）→ 直接处理不走状态机
  · 其他事件（MSG_TICK_1S / MSG_TICK_200MS）→ 分发给当前状态处理
```

**设计精妙处**：
- **状态即回调**：每个状态是 `(msg) => void` 函数——进入时调用一次（msg=NULL），此后事件驱动
- **慢闪=1s tick、快闪=200ms tick**：复用事件总线节拍，不建任务/定时器
- **上位机变量**：`led_state`（只读）+ `writable`（可写演示）注册 HOSTLINK

## 四、设计亮点

1. **序列驱动蜂鸣**：on/gap 交替数组——任意节奏（OTA 旋律/按键反馈统一接口）
2. **阻塞/非阻塞双模式**：OTA 关键时序用阻塞（不依赖 Tmr Svc），日常用定时器
3. **按键消抖与长短按**：软件 5 次采样消抖 + 时长分类
4. **LED 状态机表**：状态函数表 + 事件分发——扩展新状态=加一行
5. **事件总线广播**：KEY 事件多订阅（GUI/蜂鸣/LED）互不冲突

## 五、待读清单（下一课——Application 收官 + BSP）

- [x] buzzer / key / led（本轮完成）
- [ ] `ctrl/`：pid(539) / filter(371) / kalman(760) / test_ctrl(743)——控制算法库
- [ ] `data_agent.c`：数据代理
- [ ] `cmd_can.c` / `cmd_catalog.c` 剩余命令
- [ ] 之后转 BSP 层（lcd / w25q128 / uart / can / es8388 / touch / mpu6050 / eeprom / sram / power / i2s / system）

## 六、自测题

1. Buzzer_PlaySequence 为什么必须拷贝 2n 项？（断言事故）
2. OTA 旋律为什么用阻塞模式？（Tmr Svc 可能被饿死）
3. 有源蜂鸣器如何表达音高？（节奏即音高）
4. 按键长短按怎么判定？（消抖 5 次 + 时长 10ms~1s / ≥1s）
5. LED 状态机表的结构？（状态函数表 + msg=NULL 进入动作）
6. 慢闪快闪如何实现？（复用 1s/200ms 事件总线节拍）
7. 为什么 LED 状态切换要同步上位机变量？
8. KEY 事件多订阅为什么互不冲突？（广播语义）
