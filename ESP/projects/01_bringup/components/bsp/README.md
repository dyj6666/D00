# BSP 组件 —— 板级支持包（ATK-DNESP32S3B / ESP32S3 BOX）

> 目标：把这块板**所有外设**收敛成一套分层清晰、依赖自动解析、可回滚、
> 线程安全、且已经按性能最优方式实现过的驱动集合。
> 应用层只写业务，不碰寄存器、不碰裸引脚号。

---

## 一、分层与目录

```
components/bsp/
├── include/bsp/
│   ├── bsp.h               总入口：bsp_init/bsp_deinit/bsp_print_info/bsp_selftest
│   ├── bsp_types.h         公共类型、错误约定、模块位掩码、BSP_MOD_ALL
│   ├── bsp_pinmap.h        ★ 全板引脚/地址"单一事实源"（改硬件只改这里）
│   ├── bus/i2c_bus.h       I2C0 共享总线服务（互斥 + 统计 + 设备管理 + 探测）
│   ├── bus/spi_bus.h       SPI2 共享总线服务
│   ├── xl9555.h            16 位 IO 扩展（背光/蜂鸣器/按键/红LED/触摸复位/功放使能）
│   ├── lcd.h              2.4" 320x240 TFT（8080 并口 ST7789）+ 背光 + 帧缓冲
│   ├── touch.h            电容触摸 CHSC5432（5 点，1 次 I2C 读 28 字节）
│   ├── eeprom.h           AT24C02（跨页写 + ACK 轮询）
│   ├── lightsensor.h      AP3216C（光/接近/红外，1 次事务读 6 字节）
│   ├── imu.h              QMI8658A（±g / dps 可配，1 次事务读 12 字节）
│   ├── audio.h            I2S + ES8311 / NS4168 自动识别，播放/录音
│   ├── tfcard.h           TF 卡（SDSPI + FATFS，挂载 /sdcard）
│   ├── led.h              蓝灯（LEDC 硬件 PWM/渐变/呼吸）+ 红灯（IO 扩展）
│   ├── key.h              BOOT + KEY0 + KEY1（中断唤醒 + 消抖 + 短按/长按事件）
│   ├── beep.h             有源蜂鸣器（非阻塞响铃/序列）
│   ├── adc.h              扩展排针 ADC（IO5/IO6/IO8，12bit + 校准）
│   ├── temperature.h      片内温度传感器
│   ├── rtc.h              片内 RTC / 系统时间（时区 + 格式化）
│   ├── uart.h             外部串口（默认 UART1 @ IO6/IO5，事件队列模型）
│   └── ui_font.h          板载点阵字库（ASCII 8x16 + 汉字 16x16，自动生成）
└── src/                   与上表一一对应的实现
```

**三层职责**

| 层 | 目录 | 只做什么 | 不做什么 |
| --- | --- | --- | --- |
| 总线层 | `bus/` | 拥有总线句柄、加锁、超时、设备挂载、总线统计 | 不懂具体器件协议 |
| 器件层 | `*.c`（chip） | 和芯片对话：寄存器读写、单位换算 | 不做策略（消抖/闪烁/音量曲线） |
| 设备层 | 同上（对外的语义 API） | 面向应用的语义：呼吸灯、按键事件、帧缓冲、音量 | 不碰寄存器细节 |
| 板级层 | `bsp.c` | 模块表、依赖解析、按序初始化、失败回滚、自检打印 | 不实现具体外设 |

---

## 二、快速上手

```c
#include "bsp/bsp.h"
#include "bsp/lcd.h"
#include "bsp/imu.h"
#include "bsp/key.h"

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    /* 一次把所有板载外设拉起来（内部自动排序 + 依赖补齐 + 失败回滚） */
    ESP_ERROR_CHECK(bsp_init(BSP_MOD_ALL));

    bsp_print_info();       /* 模块状态 + I2C 扫描 + 关键引脚，一眼看清板子 */
    bsp_selftest(true);     /* 只读自检：谁在线、谁没应答 */

    /* 按键事件 */
    bsp_key_set_callback(on_key, NULL);     /* 在按键任务上下文回调，可阻塞 */

    /* IMU 读数 */
    bsp_imu_data_t imu;
    if (bsp_imu_read(&imu) == ESP_OK) {
        ESP_LOGI("APP", "acc=(%.2f,%.2f,%.2f)g gyro=(%.1f,%.1f,%.1f)dps %.1fC",
                 imu.acc_g.x, imu.acc_g.y, imu.acc_g.z,
                 imu.gyro_dps.x, imu.gyro_dps.y, imu.gyro_dps.z, imu.temp_c);
    }

    /* 屏幕：往帧缓冲里画，最后 flush 一次 */
    uint16_t *fb = bsp_lcd_fb();
    bsp_lcd_fb_fill_rect(0, 0, BSP_LCD_WIDTH, BSP_LCD_HEIGHT, BSP_LCD_COLOR_BLACK);
    bsp_lcd_fb_fill_rect(10, 10, 100, 40, BSP_RGB565(255, 128, 0));
    bsp_lcd_flush();
}
```

只初始化需要的部分也完全可以（依赖会被自动带上）：

```c
bsp_init(BSP_MOD_LCD | BSP_MOD_IMU);    /* 实际会连带 I2C + XL9555 */
```

---

## 三、外设与引脚一览（来源见 `bsp_pinmap.h`）

| 外设 | 型号 | 接口 / 引脚 | 说明 |
| --- | --- | --- | --- |
| 显示 | ST7789VW 320x240 | 8080 8bit：CS1 DC2 WR42 RD41 D0~D7=40/39/38/12/11/10/9/46 | 整帧缓冲在 PSRAM |
| 背光 | — | XL9555 **P0_7** | 只能开/关（无 PWM） |
| 触摸 | CHSC5432 | I2C0 0x2E，RST=XL9555 P0_6，INT=P1_1 | 最多 5 点 |
| IO 扩展 | XL9555 | I2C0 0x20，INT=IO3 | 16 位全用满 |
| EEPROM | AT24C02 | I2C0 0x50 | 256B / 8B 页 |
| 光环境 | AP3216C | I2C0 0x1E，INT=XL9555 P0_0 | 全开采样间隔 >112.5ms |
| 六轴 | QMI8658A | I2C0 0x6A，INT=XL9555 P0_1 | 默认 ±8g / ±512dps / 500Hz |
| 音频 | ES8311 或 NS4168 | I2S0：BCLK21 WS13 DOUT14 DIN47（无 MCLK） | 运行时自动识别 |
| 功放使能 | — | XL9555 **P0_5** | 上电默认关 |
| 麦克风 | MSM261S4030H0R | I2S0 DIN（IO47） | 数字 MEMS |
| TF 卡 | Micro SD | SPI2：SCK7 MISO15 MOSI16 CS17 | 20MHz，/sdcard |
| 蓝灯 | 0805 | IO4（低电平点亮） | LEDC PWM + 硬件渐变 |
| 红灯 | 0805 | XL9555 **P1_0**（低电平点亮） | 仅开/关 |
| 蜂鸣器 | 有源 | XL9555 **P0_2**（高=关） | 非阻塞响铃 |
| 按键 | BOOT / KEY0 / KEY1 | IO0 / XL9555 P0_4 / P0_3（低有效） | 中断唤醒 + 20ms 消抖 |
| 扩展 IO | — | IO5 / IO6 / IO8 / IO18；XL9555 P1_2~P1_7 | IO5/6/8 可做 ADC |
| 外部串口 | — | UART1 @ TX=IO6 / RX=IO5（默认） | 供板间通信 |

---

## 四、性能上的关键取舍（都写在对应源码注释里）

| 场景 | 做法 | 相对"常见写法"的收益 |
| --- | --- | --- |
| LCD 上屏 | 整帧帧缓冲（PSRAM，64B 对齐）+ **单次 DMA** + 完成回调等待 | 240 次小传输 → 1 次；且彻底避开"提交即返回"导致的缓冲区竞态（见 ENGINEERING_LOG 第 1 条） |
| AP3216C | 6 字节 **一次事务**读完 | 厂商例程是逐字节读 6 次并每次 delay 10ms（>60ms）→ 现在约 0.2ms |
| QMI8658A | 12 字节一次事务；灵敏度按量程算，不查表 | 一次事务拿到全部数据 |
| EEPROM 写 | 跨页自动切分 + **ACK 轮询**代替固定 5ms 延时 | 连续多页写快 2~3 倍 |
| XL9555 | 影子寄存器 + 掩码写，P0/P1 **一次事务**同时更新 | 改一位也只发 3 字节，不做"读-改-写" |
| 蓝灯亮度/呼吸 | LEDC **硬件渐变** | 渐变过程 CPU 零占用 |
| 按键 | 中断唤醒任务；**只有按下期间**才 10ms 扫描 | 空闲时 CPU 占用为零，不用轮询 |
| 蜂鸣器响铃 | `esp_timer` 一次性/序列驱动 | 调用方不必阻塞 |
| 串口 | 驱动缓冲区 + 事件队列 | 无需轮询，中断里不做重活 |
| I2C 错误 | 日志限流（前 3 条 + 每 64 条一次） | 器件掉线时不会把串口和 CPU 拖死 |
| 屏上文字 | 1bpp 点阵查表 + **只刷改动区域**（`bsp_lcd_flush_rect()`） | 改一行只发一行，实时状态条不闪、CPU 占用可忽略 |

---

## 五、约定与约束

1. **幂等**：所有 `init` 重复调用安全；`bsp_init` 只启动"未运行"的模块。
2. **依赖自动补齐**：见 `bsp.c` 的 `s_deps[]`；加模块时同步补依赖即可。
3. **失败回滚**：`bsp_init` 中途失败会倒序关掉本次已启动的模块。
4. **可选模块**：TF 卡这类可插拔件失败只警告（表里 `optional = true`）。
5. **线程安全**：共享总线内部有锁；跨任务的驱动调用是安全的。
   例外与注意点都写在各头文件里（例如 XL9555 的 ISR 回调只能做 ISR 安全操作）。
6. **回调上下文**：`bsp_key_*` 回调在按键任务里；`bsp_xl9555_attach_int` 回调在中断里。
7. **颜色**：统一用 `BSP_LCD_COLOR_*` 或 `BSP_RGB565(r,g,b)`。

---

## 六、怎么加一个新外设（3 步）

1. `bsp_pinmap.h` 里补引脚/地址（唯一改动点）。
2. 新增 `include/bsp/xxx.h` + `src/xxx.c`，实现 `bsp_xxx_init()/bsp_xxx_deinit()`；
   如果挂在 I2C/SPI 上，直接用 `bsp_i2c_add_device()` / `bsp_spi_add_device()`，
   不要自己初始化总线。
3. `bsp.c` 的 `s_modules[]` 加一行；有依赖再在 `s_deps[]` 加一行；
   `bsp_types.h` 补一个 `BSP_MOD_XXX` 位。

---

## 七、已知限制

- **ES8311 只支持 32k/44.1k/48k/64k/88.2k/96kHz**：板上没有 MCLK，
  编解码用 BCLK 当 MCLK，BCLK 必须是采样率×32（16bit 立体声），
  因此 16kHz 录音需要改用 32bit 槽宽（或外接 MCLK）。
- **NS4168 无软件音量**：只有硬件固定增益，音量请在上层缩放采样数据。
- **触摸方向**：默认按横屏换算，若左右/上下颠倒，改 `BSP_TOUCH_LANDSCAPE`
  或用 `bsp_touch_read_raw()` 看原始值定位。
- **片内 RTC 无备电**：完全断电后时间丢失，需要重新校准。
- **TF 卡默认不格式化**：挂载失败不会动卡上的数据；需要时调用
  `bsp_tfcard_format_if_failed(true)`。
