# D00 源码研学 · 笔记 33 —— BSP 杂项批（watchdog/flash/buzzer/rtc）

> 精读对象：`bsp_watchdog.c`(17) · `bsp_flash.c`(119) · `bsp_buzzer.c`(42) · `bsp_rtc.c`(56)
> 阶段 3 BSP 第 7 批：小而关键的系统级驱动

---

## 一、看门狗（bsp_watchdog.c，17 行）

```c
void BSP_Watchdog_Refresh(void)
{
#if APP_DEBUG_MODE
    /* 调试构建：IWDG 未启动，无需喂狗 */
#else
    HAL_IWDG_Refresh(&hiwdg);
#endif
}
```
- **APP_DEBUG_MODE 编译期切换**：调试构建（gdb 断点）不喂狗不复位——发布构建恢复
- 全项目统一喂狗入口（SysTick 钩子/长操作/锁定等待全走这里）

## 二、⭐ 片内 Flash（bsp_flash.c）—— APP 侧 Flash 服务

### 2.1 扇区映射（:13-27）
与 BOOT flash_if.c 相同的 12 扇区映射（0-3×16KB，4-11×128KB）——**两个独立实现保持一致**（分区约定 AGENTS.md）。

### 2.2 三个要点
| 函数 | 实现 | 血泪 |
| --- | --- | --- |
| EraseRange | 逐扇区 HAL 擦除 + **清全部错误标志**（含 OPTERR/SOP，RDP 解除残留） | 不清 HAL 直接 HAL_ERROR |
| **ProgramWord** | **逐字编程短暂关中断**（:59）——"CAN 1Mbps 连续帧下 FIFO 可在两字之间排空" | 防与其它 Flash 访问交错 |
| Write | 前导/尾部非对齐**读-改-写** + 整字段 memcpy | 与 BOOT flash_write 同款 |

### 2.3 其他
- `ResetController`（:106-114）：清标志 + 解锁/锁定——**Ota_Begin 续传路径重置 Flash 控制器状态**（防残留导致编程 BSY 卡死，笔记 08 呼应）
- `GetStatusSR`：SR 读取诊断

## 三、蜂鸣器（bsp_buzzer.c，42 行）

**有源蜂鸣器 GPIO 高电平驱动（PF8）**：On/Off/Toggle——buzzer_app 的"节奏即音高"靠 GPIO 时序（非 PWM，有源自带振荡）。

## 四、RTC 抽象（bsp_rtc.c，56 行）

| 接口 | 要点 |
| --- | --- |
| Write/ReadBackupReg | **HAL 第二参数是索引 0..19 非地址**（注释明示）——BKP 分区约定（reg0=OTA/1-15=崩溃/16-19=BOOT） |
| SetDateTime | HAL_RTC_SetTime + SetDate（BIN 格式；年取 mod 100） |
| GetDateTime | 2000+Year 还原；SNTP 校准入口（笔记 18 呼应） |

## 五、设计亮点

1. **DEBUG 编译期喂狗切换**：调试不断点复位、发布不裸奔
2. **Flash 错误标志清零纪律**：RDP 解除残留（OPTERR/SOP）必清——BOOT/APP 同款
3. **逐字编程短关中断**：CAN 满速下 FIFO 排空窗口的工程权衡
4. **Flash 控制器复位**：续传前重置状态防 BSY 卡死
5. **BKP 索引语义**：HAL 索引 vs 硬件寄存器编号的澄清注释

## 六、待读清单（下一课——BSP 收官）

- [x] watchdog / flash / buzzer / rtc（本轮完成）
- [ ] `bsp_touch.c`(242) / `bsp_gpio.c` / `bsp_lcd.c`(444)
- [ ] `LCD/lcd.c`(1195) + `LCD/lcd_ex.c`(1568)：LCD 驱动
- [ ] BSP 收官 → **HOST 上位机（9.7k 行）**

## 七、自测题

1. APP_DEBUG_MODE 怎么影响喂狗？（编译期切换）
2. Flash 错误标志为什么要清 OPTERR/SOP？（RDP 解除残留）
3. 逐字编程为什么短关中断？（CAN 连续帧 FIFO 排空窗口）
4. ResetController 的调用时机？（Ota_Begin 续传路径）
5. BKP HAL 第二参数是什么？（索引 0..19 非地址）
6. 有源蜂鸣器为什么 GPIO 直驱？（自带振荡，节奏即音高）
7. GetDateTime 的年怎么还原？（2000+Year）
8. Flash 扇区映射与 BOOT 的一致性靠什么保证？（分区约定）
