# 01_bringup —— ESP32-S3-BOX（正点原子 AI BOX1）环境自检工程

> 目的：**一次编译烧录，把整条工具链和板级基础能力验证完**。
> 这是 ESP 子空间的第一个工程；跑通后再进入 WiFi / BLE / 语音 / 与 STM32 串口桥的开发。

---

## 一、硬件与工具链

| 项 | 值 |
| --- | --- |
| 开发板 | 正点原子 AI BOX1（ESP32-S3-WROOM-1-**N16R8**） |
| Flash / PSRAM | 16 MB / 8 MB Octal PSRAM @80 MHz |
| CPU | 双核 240 MHz |
| 串口 | **COM14**（USB 串口，115200） |
| ESP-IDF | **v5.2.1** → `E:\ESP-IDF\Espressif\frameworks\esp-idf-v5.2.1` |
| Python | 3.11.2（idf-python 内置） |

---

## 二、自检内容（代码对应 `main/main.c`）

| 项 | 日志标签 | 期望结果 |
| --- | --- | --- |
| 芯片型号/核心/修订 | `[CHIP ]` | `ESP32-S3 cores=2` |
| Flash 容量 | `[FLASH]` | `size=16 MB (OK: N16R8)` |
| PSRAM 容量 | `[PSRAM]` | `size=8 MB (OK: Octal)` |
| 出厂 MAC | `[MAC  ]` | 合法的 6 字节地址 |
| 复位原因 | `[RESET]` | 首次上电为 `POWERON` |
| NVS 初始化 | `[NVS  ]` | `init OK`（WiFi/BLE 的存储底座） |
| 心跳任务 | `[BEAT ]` | 每 5s 一条，证明调度器在跑 |

---

## 三、编译烧录

### 3.1 ⭐ 推荐：一键脚本（已封装全部环境坑）

```powershell
cd D:\GIT-SPACE\D00\ESP\projects\01_bringup
.\idf.ps1 build                    # 编译
.\idf.ps1 -p COM14 flash           # 烧录
.\idf.ps1 -p COM14 monitor         # 串口监视（退出 Ctrl+]）
.\idf.ps1 -p COM14 flash monitor   # 烧录并监视
.\idf.ps1 menuconfig               # 配置菜单
```

> `idf.ps1` 自动处理两件事：**选中 IDF 自带 Python 3.11** + **强制 PYTHONUTF8**
> （原因见文末「编译验证记录与踩坑」）。

### 3.2 手工方式（了解原理用）

```powershell
# ① 先把 IDF 自带 Python 3.11 放到 PATH 最前（否则会误选系统 Python 3.12）
$env:PATH = 'E:\ESP-IDF\Espressif\tools\idf-python\3.11.2;E:\ESP-IDF\Espressif\tools\idf-python\3.11.2\Scripts;' + $env:PATH
# ② 强制 Python 用 UTF-8（否则中文 Windows 上 GBK 解码崩溃）
$env:PYTHONUTF8 = '1'

# ③ 激活 IDF 环境（每个新终端都要执行一次）
. E:\ESP-IDF\Espressif\frameworks\esp-idf-v5.2.1\export.ps1

# ④ 进入工程目录
cd D:\GIT-SPACE\D00\ESP\projects\01_bringup

# ⑤ 设定目标芯片（仅首次需要；生成 sdkconfig）
idf.py set-target esp32s3

# ⑥ 编译
idf.py build

# ⑦ 烧录 + 串口监视（COM14 按实际端口调整）
idf.py -p COM14 flash monitor
#    退出监视：Ctrl + ]
```

> **cmd 用户**：把第 1 步换成 `E:\ESP-IDF\Espressif\frameworks\esp-idf-v5.2.1\export.bat`

## 四、编译烧录（VS Code 方式）

1. VS Code 打开文件夹 `D:\GIT-SPACE\D00\ESP\projects\01_bringup`
2. `Ctrl+Shift+P` → `ESP-IDF: Set Espressif Device Target` → **esp32s3**
3. `ESP-IDF: Build your project`
4. `ESP-IDF: Flash your project`（自动选 COM14）
5. `ESP-IDF: Monitor`

---

## 五、期望日志（示例）

```
I (312) BRINGUP: ========== ESP32-S3-BOX (AI BOX1) BRINGUP ==========
I (312) BRINGUP: [CHIP ] model=ESP32-S3 cores=2 revision=v0.2
I (322) BRINGUP: [CHIP ] features: WiFi BLE emb-psram
I (332) BRINGUP: [FLASH] size=16 MB (OK: N16R8)
I (342) BRINGUP: [PSRAM] size=8 MB (OK: Octal)
I (352) BRINGUP: [MAC  ] STA xx:xx:xx:xx:xx:xx
I (362) BRINGUP: [RESET] reason=POWERON (上电/复位键)
I (372) BRINGUP: [HEAP ] internal free=385xxx B, min=380xxx B
I (382) BRINGUP: [NVS  ] init OK
I (392) BRINGUP: bringup done. Next: WiFi / BLE / UART bridge to STM32.
I (5392) BRINGUP: [BEAT ] #1 uptime=5 s | heap free=385xxx B min=380xxx B
```

> ⚠️ 日志含中文，请用 `idf.py monitor` 或 VS Code 串口监视（UTF-8）查看；
> 用普通串口助手（GBK）会乱码（功能不受影响）。

---

## 六、配置说明（`sdkconfig.defaults`）

关键项（值取自正点原子官方例程，勿随意改动）：

| 配置 | 值 | 说明 |
| --- | --- | --- |
| `CONFIG_IDF_TARGET` | `esp32s3` | 目标芯片 |
| `CONFIG_ESPTOOLPY_FLASHSIZE` | `16MB` | N16R8 的 Flash |
| `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` | `partitions-16MiB.csv` | 16MB 分区表（见下） |
| `CONFIG_SPIRAM` + `MODE_OCT` + `SPEED_80M` | y | 8MB Octal PSRAM（LVGL/音频必备） |
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` | `240` | 双核 240MHz |
| `CONFIG_ESP_CONSOLE_UART_BAUDRATE` | `115200` | 控制台波特率 |
| `CONFIG_FREERTOS_HZ` | `1000` | 与例程一致的节拍 |

**分区表（`partitions-16MiB.csv`）**：

| 名称 | 类型 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 24 KB | WiFi/BLE 凭据、用户参数 |
| phy_init | data/phy | 0xF000 | 4 KB | RF 校准 |
| factory | app | 0x10000 | ~2 MB | 应用固件 |
| vfs | data/fat | 0x200000 | 10 MB | FAT 文件系统（SD 卡镜像/资源） |
| storage | data/spiffs | 0xC00000 | 4 MB | SPIFFS（图片/音频资源） |

---

## 七、目录结构

```
01_bringup/
├── CMakeLists.txt          顶层工程（project(box_bringup)）
├── sdkconfig.defaults      板级默认配置（16MB + Octal PSRAM + 240MHz）
├── partitions-16MiB.csv    16MB 分区表
├── idf.ps1                 一键环境脚本（纯 ASCII，原因见 10.6 ④）
├── tools/
│   ├── gen_font.py         名字卡汉字字模生成（Pillow → 8bit 覆盖率位图）
│   ├── gen_ui_font.py      UI 点阵字库生成（ASCII 8x16 + 汉字 16x16，自动扫源码补字）
│   └── check_font.py       字模数据自检（把 C 数组反向解析回位图）
├── components/
│   └── bsp/                ★ 板级支持包（独立 IDF 组件，全部板载外设驱动）
│       ├── README.md       分层说明 / 外设引脚一览 / 性能取舍 / 扩展指南
│       ├── include/bsp/    对外头文件（bsp.h、bsp_pinmap.h、各外设 .h）
│       └── src/            实现（bus/ 总线层 + 各器件/设备驱动）
├── main/
│   ├── CMakeLists.txt      组件注册（不写 REQUIRES，原因见 10.6 ⑤）
│   ├── main.c              自检固件（芯片/Flash/PSRAM/MAC/NVS/心跳 + 名字卡）
│   └── app/
│       ├── boot_ui.c/.h    开机自检动画（屏幕逐项出结果 + 实时状态条）
│       ├── name_card.c/.h  名字卡渲染（抗锯齿，整帧渲染 + 单次上屏）
│       └── font_name_64.c/.h  名字卡汉字字模（自动生成，勿手改）
├── ENGINEERING_LOG.md      重点问题复盘（按 docs\ISSUE_POSTMORTEM_TEMPLATE.md）
└── README.md               本文件
```

> 首次 `idf.py build` 会自动生成 `sdkconfig` 与 `build/`（均不入 git）。

---

## 八、跑通后的下一步（对齐 ESP/README.md 的开发计划）

| 步骤 | 内容 | 参考例程（原盘） |
| --- | --- | --- |
| ① | UART 收发验证 → 与 STM32 串口桥雏形 | `04_uart` |
| ② | WiFi STA 连接 → HTTP/MQTT 上云 | 扩展例程 `2，WiFi例程` |
| ③ | BLE 数据通道 | 乐鑫官方 `bluetooth/` 例程 |
| ④ | LCD 显示 + LVGL 界面 | `11_lcd` / `30_lvgl_demo` |
| ⑤ | 语音（麦克风采集 / 唤醒词） | `24_recoding` |
| ⑥ | 与 D00 项目 MCU 联调（UART 协议对齐 cam_link 风格） | 见 `APP/Config/pinout.h` 预留 PC6/PC7 |

**原盘例程路径**：
`D:\【正点原子】ESP32 AI BOX1 资料（A盘）\4，程序源码\v_5.2.1版本例程\1，标准例程-IDF版\basic_routines\`

---

## 九、验证记录与踩坑（2026-09-29 实测）

### 9.1 编译结果 ✅

```
Bootloader binary size 0x5250 bytes. 0x2db0 bytes (36%) free.
Successfully created esp32s3 image.
Generated build/box_bringup.bin
box_bringup.bin binary size 0x3bf30 bytes. Smallest app partition is 0x1f0000 bytes. 0x1b40d0 bytes (88%) free.
Project build complete.
```

### 9.2 烧录 + 启动自检 ✅（真机验证）

烧录：三个镜像 `Hash of data verified`，`Hard resetting via RTS pin...`

启动日志（COM14 @115200 实测节选）：
```
I (44)  boot.esp32s3: SPI Flash Size : 16MB
I (249) esp_psram: Found 8MB PSRAM device
I (253) esp_psram: Speed: 80MHz
I (710) esp_psram: SPI SRAM memory test OK
I (719) cpu_start: cpu freq: 240000000 Hz
I (722) cpu_start: Project name: box_bringup
I (871) BRINGUP: [NVS  ] init OK
I (872) BRINGUP: [CHIP ] model=ESP32-S3 cores=2 revision=v0.2
I (883) BRINGUP: [FLASH] size=16 MB (OK: N16R8)
I (888) BRINGUP: [PSRAM] size=8 MB (OK: Octal)
I (893) BRINGUP: [MAC  ] STA 50:78:7D:17:05:80
I (898) BRINGUP: [RESET] reason=POWERON (上电/复位键)
I (924) BRINGUP: [BEAT ] #1 uptime=0 s
I (5932)  BRINGUP: [BEAT ] #2 uptime=5 s
I (10932) BRINGUP: [BEAT ] #3 uptime=10 s
```

**结论：芯片识别、Flash 16MB、PSRAM 8MB Octal @80MHz、240MHz 主频、NVS、调度器心跳——全部与配置相符。**

### 9.3 踩坑记录（三个真实问题）

| # | 现象 | 根因 | 解法 |
| --- | --- | --- | --- |
| ① | `export.ps1` 报 `python_env\idf5.2_py3.12_env\Scripts\python.exe doesn't exist`，随后 `Cannot import module "click"` | 系统 PATH 里有 `D:\Python`（Python 3.12），export 用它推算虚拟环境名；但实际虚拟环境是 `idf5.2_py3.11_env`（用 IDF 自带 Python 3.11.2 建的） | 调 export 前把 `...\tools\idf-python\3.11.2`（及其 `Scripts`）放到 PATH 最前；VS Code 侧配 `idf.pythonInstallPath` |
| ② | `UnicodeDecodeError: 'gbk' codec can't decode byte 0x..`（先是 kconfgen，后是 idf_py_actions hints） | Python 在中文 Windows 上默认用 **GBK** 读写文件/输出；`sdkconfig.defaults` 的 UTF-8 中文注释和构建日志里的中文都会撞上 | ①`sdkconfig.defaults` **保持纯 ASCII**；②设 `PYTHONUTF8=1`（命令行）与 `idf.customExtraVars`（VS Code） |
| ③ | `error: 'ESP_RST_EFUSE' undeclared`（连同 `PWR_GLITCH`/`CPU_LOCKUP`） | `esp_reset_reason_t` 的这三个枚举是 **IDF 5.3+** 才加的，5.2.1 没有 | 删掉这三个 case，用 `default:` 兜底 |

### 9.4 结论

- **工具链、板级配置、工程骨架、烧录链路、自检固件全部验证通过**（编译 ✅ 烧录 ✅ 运行 ✅）
- 三条踩坑已固化到 `idf.ps1` / `.vscode/settings.json` / `sdkconfig.defaults`（不需重复排查）
- **注意**：`sdkconfig.defaults` / `Kconfig` 类文件今后**禁止写入非 ASCII 字符**（中文注释也不行）
- **VS Code 配置与调试方法**：见 `VSCODE_GUIDE.md`

---

## 十、开机名字卡：在 LCD 上显示汉字（2026-10-13 新增）

### 10.1 做了什么

上电后 LCD 居中显示「**董衍俊**」三个汉字：白底、深蓝字、64×64 抗锯齿。

### 10.2 硬件链路（沿用正点原子 ESP32S3 BOX 约定，勿改）

| 项 | 值 |
| --- | --- |
| 面板 | 3.2" 320×240，ST7789 控制器，**8080 并口（8bit MCU 屏）** |
| 并口引脚 | CS=IO1、DC=IO2、RD=IO41、WR=IO42、RST=未接；D0~D7=IO40/39/38/12/11/10/9/46 |
| 背光 | **不在 GPIO 上**，挂 XL9555 的 P07（I2C0：SDA=IO48、SCL=IO45、7bit 地址 0x20） |

> 两个关键点：
> 1. 必须先用 I2C 打开 XL9555 的背光，否则屏是全黑，很容易误判成"面板初始化失败"。
> 2. 8bit 并口一个像素要分两次写，面板 IO 必须开 `flags.swap_color_bytes`；开之后缓冲区里放标准 RGB565 即可，不用自己倒字节。

### 10.3 新增文件

| 文件 | 作用 |
| --- | --- |
| `main/bsp/xl9555.c/.h` | I2C0 + XL9555 最小驱动（初始化 + 单引脚写，目前只用于背光） |
| `main/bsp/lcd.c/.h` | 8080 总线 + ST7789 面板 + 背光控制；对外 `lcd_fill()` / `lcd_draw_bitmap()` |
| `main/app/name_card.c/.h` | 名字卡渲染：整屏逐行扫描，每行一次 DMA，抗锯齿用覆盖率查表 |
| `main/app/font_name_64.c/.h` | 汉字字模（**自动生成，勿手改**） |
| `tools/gen_font.py` | 字模生成脚本：用 Pillow 把汉字渲染成 8bit 覆盖率位图 |

### 10.4 改名字 / 换字号 / 换配色

1. 编辑 `tools/gen_font.py` 顶部的 `CHARS`（要显示的字）、`GLYPH_PX`（字号）、`FG_RGB`/`BG_RGB`（配色）
2. 运行 `D:\Python\python.exe tools\gen_font.py`，重新生成 `main/app/font_name_64.c/.h`
3. 若改了字间距 `SPACING_PX`，同步改 `main/app/name_card.c` 里的 `NAME_GLYPH_GAP`
4. 脚本会在 `build/font_preview/preview_screen.png` 输出一张 320×240 预览图，**先看图再烧录**，省一次来回

### 10.5 为什么不用 LVGL / SD 卡字库

bringup 阶段只需要 3 个汉字。引入 LVGL + 全量 GBK 字库（或官方例程那种 SD 卡字库）的体积、
依赖和失败点都远大于收益；所以把用到的字形离线渲染成位图直接编进固件
（3 × 64 × 64 = 12 KB），`main` 里没有任何新增第三方依赖。

### 10.6 本次新增踩坑（2026-10-13）

| # | 现象 | 根因 | 解法 |
| --- | --- | --- | --- |
| ④ | `.\idf.ps1 build` 必然报 `idf.py : 无法将"idf.py"项识别为 cmdlet…`，而且 export.ps1 一句输出都没有 | `idf.ps1` 是**无 BOM 的 UTF-8** 且含中文注释；Windows PowerShell 5.1 对无 BOM 脚本按 ANSI(GBK) 解码，多字节歧义会把 `. export.ps1` 那一行**吞掉**（并进上一条注释），于是 `idf.py` 函数从未被定义 | `idf.ps1` 注释改为**纯 ASCII**（与 `sdkconfig.defaults` 同一条纪律）。另外**不要**写 `. script \| Out-Null`——管道会给脚本另开作用域，函数定义同样会丢 |
| ⑤ | 加了 LCD 代码后编译报 `main.c:25:10: fatal error: esp_flash.h: No such file or directory` | 我给 `main` 组件**手工写了 `REQUIRES`**。IDF 只在 main"没有手工声明依赖"时，才让它自动依赖本次构建的全部组件（`tools/cmake/project.cmake` 的 "Special treatment for 'main' component"）；一旦显式声明，main 就只剩列出来的那几个 | `main/CMakeLists.txt` **不写 REQUIRES/PRIV_REQUIRES**，只加 `SRC_DIRS` / `INCLUDE_DIRS` |
| ⑥ | 上屏后只有第一个字「董」正常，后面两个字是白的（背景色），像没画上去 | i80 面板 IO 是**"提交即返回"**的：`panel_io_i80_tx_color()` 里 `trans_desc->data = color` 直接指向调用方的缓冲区，且最多可排队 `trans_queue_depth`(10) 笔，DMA 真正搬运发生在中断里。逐行刷屏时每一行都复用了同一个行缓冲，**下一行的写入会盖掉正在被搬运的数据** | 改成"整帧渲染进 PSRAM 帧缓冲 → 一次 `draw_bitmap` 整屏推送 → `lcd_wait_flush_done()` 等这次 DMA 真结束"；`lcd_fill()` 也改成逐行等完成。提交前还会清掉残留完成信号，保证等的是"这一次" |

> 另外：调试期若构建"卡死"且 `build\.ninja_log` 被占用，多半是上一次被中断的
> `python` / `ninja` / `cmake` 进程还活着（用任务管理器结束即可）。ccache 在受限环境下
> 也可能因为缓存目录不可写而阻塞，可用 `.\idf.ps1 --no-ccache build` 绕过。

### 10.7 验证状态（2026-10-13 实机通过）

- [x] 编译通过（`.\idf.ps1 build`，784/784）
- [x] 烧录后屏幕完整显示「董衍俊」，日志有 `LCD: init OK` 与
      `NAMECARD: rendered "董衍俊": 3 glyphs 64x64, area x=54..265 y=88..151 (frame=1)`
- [x] 自检边框（`NAME_CARD_DIAG_FRAME`）四边齐全，确认整屏可达

> 完整排查过程（含四个被推翻的错误方向：字模为空 / 指针错 / 驱动截断 / 配置抄错）
> 见 [ENGINEERING_LOG.md](ENGINEERING_LOG.md) 第 1 条。

---

## 十一、BSP 板级支持包（2026-10-13 新增）

原来的 `main/bsp/` 只有 LCD 和 XL9555 两个驱动，已升级为**独立的 IDF 组件**
`components/bsp/`，把这块板**所有外设**都收敛进去，应用层不再碰寄存器和裸引脚号。

**分层**

| 层 | 位置 | 职责 |
| --- | --- | --- |
| 总线层 | `components/bsp/src/bus/` | I2C0 / SPI2 共享总线：句柄、加锁、超时、设备挂载、统计 |
| 器件层 | `components/bsp/src/*.c` | 和芯片对话（寄存器、单位换算），不含策略 |
| 设备层 | 同上，对外的语义 API | 呼吸灯 / 按键事件 / 帧缓冲 / 音量 / 响铃 |
| 板级层 | `components/bsp/src/bsp.c` | 模块表 + 依赖解析 + 按序初始化 + 失败回滚 + 自检 |

**覆盖的外设**：LCD（8080+ST7789+背光+帧缓冲）、电容触摸 CHSC5432、XL9555 IO 扩展、
AT24C02 EEPROM、AP3216C 光环境、QMI8658A 六轴、ES8311/NS4168 音频（自动识别）、
TF 卡（SDSPI+FATFS）、蓝/红 LED、BOOT/KEY0/KEY1 按键、有源蜂鸣器、
扩展排针 ADC、片内温度、片内 RTC、外部串口 —— **无遗漏**。

**用法（详见 `components/bsp/README.md`）**

```c
ESP_ERROR_CHECK(bsp_init(BSP_MOD_ALL));   // 自动排序 + 依赖补齐 + 失败回滚
bsp_print_info();                         // 模块状态 + I2C 扫描 + 关键引脚
bsp_selftest(true);                       // 只读自检，谁在线一目了然
```

**注意**：`main/CMakeLists.txt` 依旧**不写 REQUIRES**；
`components/bsp` 作为独立组件自己声明 REQUIRES，
而 `main` 是"不写依赖就自动依赖全部组件"的特殊组件，所以照样能直接用（见 10.6 ⑤）。

---

## 十二、开机自检动画（2026-10-13 新增）

> 目的：**不用连串口看终端**也能完成全部验收。屏幕自己把每一项驱动的自检过程与结果演一遍。

### 12.1 屏幕上的流程

| 阶段 | 画面 | 说明 |
| --- | --- | --- |
| ① 屏幕自检 | 整屏快速闪过 白 → 红 → 绿 → 蓝（各 120ms） | 确认面板、背光、整屏每个像素都可达 |
| ② 逐项自检 | 黑色列表，每跑完一项落一行：`标签 + 状态 + 细节`；列表满 10 行自动上滚；底部进度条同步推进 | 状态用颜色区分：**绿=通过 / 红=失败 / 黄=待确认 / 灰=跳过** |
| ③ 收尾常驻 | 中央「董衍俊」大字卡（带 1px 自检边框）+ 底部实时状态条 | 状态条每秒刷新：时间 / 加速度 / 光照·接近·红外 / 芯片温度·角速度；最后一行显示触摸坐标或按键事件 |

自检项共 15 个：屏幕、触摸、按键、灯光、蜂鸣、扩展(XL9555)、存储(EEPROM)、光感(AP3216C)、
六轴(QMI8658A)、温度、时钟、采样(ADC)、串口、卡(TF)、音频。

### 12.2 交互

| 动作 | 效果 |
| --- | --- |
| K1 短按 | 重跑一遍开机自检动画 |
| K2 短按 | 短鸣一声 |
| BOOT 短按 | 翻转红色 LED |
| BOOT 长按 1s | 开/关蓝色呼吸灯 |
| 手指按屏 | 状态条最后一行实时显示 `触摸 N点 (x,y)` |

### 12.3 需要人工确认的项（屏幕上标"待确认"）

- **触摸**：自检时会等 1.6 秒，按一下屏就变"通过"，并在状态条显示坐标；不按就保持"待确认"。
- **按键**：按 K1/K2/BOOT，状态条会显示 `按键 K1 单击` 之类的事件。
- **采样(ADC)**：扩展排针 IO5/IO6/IO8 接 3.3V 或 GND 才能确认（悬空是漂动值）。
- **串口**：把 IO6(TX) 与 IO5(RX) 短接，重测时该项会从"待确认"变"通过"（内部发 PING 校验回读）。
- **音频**：听到 440Hz 短音即通过（有声音但标"待确认"是正常的，因为驱动无法自证喇叭）。
- **灯光/蜂鸣/屏幕**：本身就是给人看的，日志与屏幕同时给结论。

### 12.4 板载字库（为什么不是 SD 卡字库）

开机动画必须"不依赖任何外部资源"就能显示文字，所以把 UI 用到的字形离线渲染成
**1bpp 点阵直接编进固件**（ASCII 8×16 共 95 个 + 汉字 16×16，约 3KB）：

- 生成工具 `tools/gen_ui_font.py`：用黑体渲染 + 阈值化 + 位打包；
- **自动补字**：脚本会扫描 `main/app/boot_ui.c` 里字符串字面量中的汉字（先剥注释），
  与显式列表取并集——屏幕上出现过的字，字库里一定有；
- 绘制 API 在 BSP 里：`bsp_lcd_text()` / `bsp_lcd_textf()` / `bsp_lcd_text_right()`，
  未收录的字会画成实心方块（缺字一眼可见，而不是静默不显示）；
- 改了屏幕上的文案后，重跑一次 `gen_ui_font.py` 即可（不需要改任何 C 代码）。
