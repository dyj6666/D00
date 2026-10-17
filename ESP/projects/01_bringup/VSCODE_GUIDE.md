# VS Code 使用与调试验证指南（ESP32-S3-BOX / 01_bringup）

> 本文覆盖：VS Code 环境配置原理 → 日常操作 → 三条调试路线 → 实测证据 → 排障。
> 环境已实测打通（2026-09-29）：编译 ✅ 烧录 ✅ 串口自检 ✅

---

## 一、环境匹配原理（为什么这样配）

### 1.1 四个关键路径（`.vscode/settings.json` 已写死）

| 配置项 | 值 | 作用 |
| --- | --- | --- |
| `idf.espIdfPathWin` | `E:/ESP-IDF/Espressif/frameworks/esp-idf-v5.2.1` | IDF 框架根 |
| `idf.toolsPathWin` | `E:/ESP-IDF/Espressif` | 工具链根（编译器/OpenOCD/cmake/ninja） |
| `idf.pythonInstallPath` | `.../tools/idf-python/3.11.2/python.exe` | ⭐ **必须显式指定**，否则扩展会用系统 Python 3.12 → 找不到虚拟环境 |
| `idf.gitPathWin` | `.../tools/idf-git/2.39.2/cmd/git.exe` | IDF 自带 git（避免系统 git 版本干扰） |

### 1.2 两个"中文 Windows 专属"的关键设置

| 设置 | 值 | 不配置的后果 |
| --- | --- | --- |
| `idf.customExtraVars` | `{"PYTHONUTF8":"1","PYTHONIOENCODING":"utf-8"}` | Python 用 GBK 解析 sdkconfig/构建日志 → `UnicodeDecodeError` 崩溃 |
| `sdkconfig.defaults` 保持纯 ASCII | — | 中文注释会让 kconfgen 直接报错（UTF-8 字节撞 GBK 解码器） |

### 1.3 目标与烧录

| 设置 | 值 | 说明 |
| --- | --- | --- |
| `idf.adapterTargetName` | `esp32s3` | 目标芯片 |
| `idf.flashType` | `UART` | 经 COM14 的 USB-UART 桥烧录 |
| `idf.portWin` / `idf.port` | `COM14` | 换 USB 口后 COM 号可能变，需同步修改 |
| `idf.flashBaudRate` | `460800` | 烧录波特率（实测稳定） |
| `idf.monitorBaudRate` | `115200` | 监视波特率（与 sdkconfig 一致） |
| `idf.openOcdConfigs` | `["board/esp32s3-builtin.cfg"]` | 调试用（见第四章） |

---

## 二、⚠️ 第一步：用正确的方式打开工程（最容易出错的地方）

### 2.1 典型错误

若在 **`ESP` 根目录**（或其它没有 CMakeLists.txt 的目录）打开 VS Code，会报：

```
CMake Error: The source directory "D:/GIT-SPACE/D00/ESP" does not appear to contain CMakeLists.txt.
终端进程 cmake.exe ... -B d:\GIT-SPACE\D00\ESP\build -S d:\GIT-SPACE\D00\ESP ... 启动失败(退出代码: 1)
```

**原因**：ESP-IDF 扩展总是以「**VS Code 打开的工作区根目录**」作为 CMake 源目录（`-S <工作区根>`），
它不看 `cmake.sourceDirectory`。所以工作区根必须**正好是含 `CMakeLists.txt` 的工程目录**。

### 2.2 两种正确打开方式

**方式 ①（推荐）：打开工作区文件**

```
双击  D:\GIT-SPACE\D00\ESP\esp32-box.code-workspace
```
工作区根自动指向 `projects/01_bringup`，以后新增工程只需在该文件的 `folders` 里追加。

**方式 ②：直接打开工程目录**

```
VS Code → File → Open Folder → D:\GIT-SPACE\D00\ESP\projects\01_bringup
```

> 打开后**判断是否成功**：左下角状态栏出现 ESP-IDF 图标与
> `[🔨 Build] [🔥 Flash] [📺 Monitor] [⚙ esp32s3] [🔌 COM14]`；若仍报上面的 CMake 错误，
> 说明工作区根还不是工程目录（用 `File → Open Folder` 重新打开）。

---

## 三、日常操作（三种等价方式）

### 方式 A：状态栏按钮（最直观）

打开工程文件夹后，VS Code 底部状态栏会出现 ESP-IDF 图标按钮：

```
[🔨 Build] [🔥 Flash] [📺 Monitor] [🧹 Clean] [⚙ Target: esp32s3] [🔌 COM14]
```

- 点 **Build** → 编译
- 点 **Flash** → 烧录
- 点 **Monitor** → 串口监视（退出：`Ctrl + ]`）

> 首次打开工程若状态栏没出现按钮：`Ctrl+Shift+P` → `ESP-IDF: Add .vscode Configuration Folder`，或等扩展激活完成（右下角会显示 ESP-IDF 图标）。

### 方式 B：任务面板（本项目已配好 8 个任务）

`Ctrl+Shift+P` → `Tasks: Run Task` → 选择：

| 任务 | 作用 |
| --- | --- |
| **ESP: Build** | 编译（也可直接 `Ctrl+Shift+B`） |
| **ESP: Flash** | 烧录到 COM14 |
| **ESP: Monitor** | 串口监视 |
| **ESP: Flash + Monitor** | ⭐ 一键烧录并监视（日常最常用） |
| ESP: Clean (fullclean) | 彻底清理 |
| ESP: Menuconfig | SDK 配置菜单 |
| ESP: Erase Flash | 擦除整片（会清 NVS，慎用） |
| ESP: Chip Info | esptool 读芯片信息 |

> 这些任务全部走 `idf.ps1`，**已固化 Python 3.11 选择与 PYTHONUTF8**，不依赖扩展状态，一定能跑。

### 方式 C：命令行（`idf.ps1`）

```powershell
cd D:\GIT-SPACE\D00\ESP\projects\01_bringup
.\idf.ps1 build
.\idf.ps1 -p COM14 -b 460800 flash monitor
```

---

## 四、⭐ 调试验证：三条路线（按推荐顺序）

### 路线 1：串口日志调试（最实用，零额外硬件）

**原理**：`idf.py monitor` 不仅能看日志，还会**自动解码 panic 崩溃栈并符号化**（把地址翻译成 `函数名 + 文件:行号`），等价于"免费的崩溃调试器"。

**用法**：
```powershell
.\idf.ps1 -p COM14 monitor
```

**验证要点**（本次实测已全部通过）：

| 检查项 | 期望输出 |
| --- | --- |
| Flash 容量 | `SPI Flash Size : 16MB` / `[FLASH] size=16 MB (OK: N16R8)` |
| PSRAM | `Found 8MB PSRAM device` + `SPI SRAM memory test OK` + `[PSRAM] size=8 MB (OK: Octal)` |
| CPU 频率 | `cpu freq: 240000000 Hz` |
| 工程身份 | `Project name: box_bringup` |
| 应用入口 | `main_task: Calling app_main()` → `BRINGUP: [NVS ] init OK` |
| 调度器运行 | `[BEAT ] #1/#2/#3 ...`（每 5 秒一条） |

**monitor 常用快捷键**：
| 键 | 作用 |
| --- | --- |
| `Ctrl + ]` | 退出 monitor |
| `Ctrl + T` `Ctrl + H` | 显示快捷键帮助 |
| `Ctrl + T` `Ctrl + R` | 复位芯片 |
| `Ctrl + T` `Ctrl + F` | 触发 core dump 到 flash |
| `Ctrl + T` `Ctrl + P` | 查看当前芯片的 panic 解码（栈回溯） |

**崩溃时怎么用**：程序 panic 后会打印 `Backtrace: 0x... 0x...`，monitor 会自动翻译成：
```
Backtrace: 0x4200abcd:0x3fc9xxxx 0x4201ef01:0x3fc9xxxx
  #0  0x4200abcd in some_function at /path/main.c:42
  #1  0x4201ef01 in app_main at /path/main.c:88
```
> 这不需要 JTAG 也不需要额外工具，把"崩溃地址 → 源码行"这一最关键的信息直接给你。

### 路线 2：GDB + OpenOCD 硬件调试（断点/单步/看变量）

**前提**：需要 JTAG 通路，即 **ESP32-S3 内置的 USB-Serial-JTAG 口**（GPIO19/20）。

#### ⭐ 本机实测结论（2026-09-29）

| 串口 | 设备身份 | 能力 |
| --- | --- | --- |
| **COM14** | `USB-Enhanced-SERIAL CH343`（VID_1A86，外置桥） | 烧录 ✅ 监视 ✅ **JTAG ❌** |
| **COM9** | `USB 串行设备`（**VID_303A = Espressif**, PID_1001, MI_00） | **烧录 ✅ 监视 ✅ JTAG ✅（全能）** |

两者 MAC 均为 `50:78:7d:17:05:80` → **同一块板子**（内置 USB 与 CH343 桥并存）。

```
$ python -m esptool --port COM9 --chip esp32s3 chip_id
Chip is ESP32-S3 (QFN56) (revision v0.2)
USB mode: USB-Serial/JTAG          ← 确认内置 USB-JTAG 通道
MAC: 50:78:7d:17:05:80
```

#### 三种 USB 口能力对照（概念澄清）

| 口类型 | 烧录 | 监视 | JTAG 调试 | 说明 |
| --- | --- | --- | --- | --- |
| 外置 USB-UART 桥（CH340/CH343） | ✅ | ✅ | ❌ | 仅串口，无调试能力 |
| **内置 USB-Serial-JTAG** | ✅ | ✅ | ✅ | **一个口顶三个**，最方便 |
| USB-OTG（TinyUSB 设备模式） | ❌ | ❌ | ❌ | 做 CDC/HID/MSC 用，与 JTAG 无关 |

> ⚠️ `USB-OTG` 与 `USB-Serial-JTAG` **共享 GPIO19/20 引脚**，但**不是同一外设**——
> JTAG 能力来自 USB-Serial-JTAG，USB-OTG 本身不能调试。同一时刻只能用其中一个外设。

#### 🏆 推荐用法：双口并行

| 用途 | 用哪个口 | 好处 |
| --- | --- | --- |
| 烧录 + 串口监视 | **COM14**（CH343） | 稳定，且不占用 JTAG 通道 |
| **硬件调试** | **COM9**（内置 USB-JTAG） | `board/esp32s3-builtin.cfg` 即为此设计（OpenOCD 经 libusb 直连，**不占 COM 号**） |

> 两个口可**同时**工作：一边用 COM14 看日志，一边用 COM9 断点调试。
> 因此 `.vscode/settings.json` 的 `idf.portWin` 保持 `COM14`（负责烧录/监视），
> 调试由 `idf.openOcdConfigs` 走 USB-JTAG，两者互不冲突。

**先判断是否可用**（若换板子，看设备管理器）：
| 设备管理器显示 | 结论 |
| --- | --- |
| `USB 串行设备` / `USB JTAG/serial debug unit`（VID_303A Espressif） | ✅ 支持，可用路线 2 |
| 仅 `USB-SERIAL CH340/CH343 (COMx)` | ❌ 仅 UART 桥，走路线 1/3 |

**用法**：
1. 插好 USB-OTG 线（见上方判断表）
2. **先编译 + 烧录**——扩展默认开启 `verifyAppBinBeforeDebug`：调试前会校验设备内固件与本地
   `build/*.bin` 是否一致，不一致会**拒绝调试**并提示 *flash the device before debugging*。
3. 按 `F5` → 选择 **`Launch (ESP-IDF)`** 配置
   - 该配置由扩展原生处理（`"type": "espidf"`），会自动拉起 OpenOCD 并连接 GDB
   - 扩展会自动在 `app_main` 处停住（等价于 `mon reset halt` + `thb app_main`）
4. 断点命中后可正常：单步（F10/F11）、看变量（Variables 面板）、调用栈（Call Stack）、内存查看

> ⚠️ **不要再用旧版 `cppdbg` 类型配置**——正点原子例程里的 launch.json 属于旧版扩展格式，
> 扩展 1.9.x 会报：`Debug configuration setting launch.configurations is invalid`。
> 本工程 `.vscode/launch.json` 已按扩展官方模板
> （`<扩展目录>\templates\.vscode\launch.json`）对齐为 `espidf` / `gdbtarget` 类型。
> 另外 JSON 文件**不能带 UTF-8 BOM**（扩展用严格解析器，BOM 会直接判为无效配置）。

**常用 GDB 命令**（在 Debug Console 输入）：
```
mon reset halt        # 复位并暂停
mon reset run         # 复位并全速运行
info registers        # 看寄存器
x/16wx 0x3fc96dc0     # 看内存
p some_variable       # 看变量
```

### 路线 3：崩溃核心转储（panic 后回溯分析，无需在线调试）

**开启**（一次性）：
```powershell
.\idf.ps1 menuconfig
# → Component config → ESP System Settings → Core dump
#   → "Core dump destination" = "Flash"
```
保存后重新编译烧录。

**崩溃后分析**：
```powershell
.\idf.ps1 -p COM14 monitor
# 触发 panic 后（或在 monitor 里按 Ctrl+T Ctrl+F 手动触发），重启时自动解码：
#   Core dump found, saving to flash...
#   ===== CORE DUMP =====
#   #0  some_function at main.c:42
#   #1  app_main at main.c:88
```

---

## 五、实测证据（2026-09-29 全链路验证）

### 4.1 芯片探测
```
$ python -m esptool --port COM14 --chip esp32s3 chip_id
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
Crystal is 40MHz
MAC: 50:78:7d:17:05:80
```

### 4.2 烧录
```
Wrote 3072 bytes ... at 0x00008000  → Hash of data verified.   (bootloader)
Wrote 3072 bytes ... at 0x00008000  → Hash of data verified.   (partition table)
Wrote 245552 bytes at 0x00010000    → Hash of data verified.   (app, 626 kbit/s)
Hard resetting via RTS pin...
```

### 4.3 启动自检（应用日志节选）
```
I (44)  boot.esp32s3: SPI Flash Size : 16MB
I (249) esp_psram: Found 8MB PSRAM device
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

**结论：工具链 / 板级配置 / 工程骨架 / 烧录链路 / 自检固件 全部验证通过。**

---

## 六、排障速查

| 现象 | 原因 | 解法 |
| --- | --- | --- |
| 状态栏没有 ESP-IDF 按钮 | 扩展未激活 / 未识别工程 | 等扩展激活；`Ctrl+Shift+P` → `ESP-IDF: Add .vscode Configuration Folder` |
| 编译报 `idf5.2_py3.12_env doesn't exist` | 扩展选中系统 Python 3.12 | 确认 `idf.pythonInstallPath` 指向 `idf-python\3.11.2\python.exe`（本工程已配） |
| 报 `UnicodeDecodeError: 'gbk'` | Python 未用 UTF-8 | 确认 `idf.customExtraVars` 含 `PYTHONUTF8`（本工程已配）；任务方式走 `idf.ps1` 也自带 |
| `Failed to connect to ESP32-S3: No serial data received` | 串口被占用 / 板子未进下载模式 | 关掉其他串口工具（monitor、串口助手）；按住 BOOT 再点 Flash |
| 烧录成功但监视无输出 | 波特率不符 / 串口选错 | monitor 用 115200；确认 `idf.portWin` 与实际一致（设备管理器查） |
| `monitor` 显示乱码 | 终端编码非 UTF-8 | 用 VS Code 集成终端或 `idf.py monitor`；本工程日志含中文，需 UTF-8 |
| `OpenOCD` 起不来 / `no device found` | USB 口不是 JTAG 口 | 换到 USB-OTG 口；或改用路线 1（串口日志调试） |
| 换 USB 口后 COM 号变了 | Windows 重新枚举 | 改 `.vscode/settings.json` 与 `tasks.json` 里的 `COM14` |

---

## 七、配置文件清单（本工程 `.vscode/`）

| 文件 | 作用 |
| --- | --- |
| `settings.json` | ESP-IDF 扩展配置（路径/芯片/串口/UTF-8 关键项） |
| `tasks.json` | 8 个任务（编译/烧录/监视/清理/配置/擦除/探测），全部走 `idf.ps1` |
| `c_cpp_properties.json` | IntelliSense（includePath 指向 IDF components + build/config） |
| `launch.json` | 调试配置（**扩展 1.9.x 格式**：`type: "espidf"` 原生调试 + `gdbtarget` 附加） |
| `c_cpp_properties.json` | IntelliSense（`compile_commands.json` 方式，**首次编译后生效**） |
