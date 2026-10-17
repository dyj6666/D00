# ESP —— ESP32 S3-BOX 开发子空间

> 正点原子 AI BOX1（ESP32-S3-WROOM-1）开发板工作区。
> 最终目标：**ESP32 ↔ STM32F407(MCU) 通信，实现 WiFi / 蓝牙 / 语音的顶级交互**
> （体感游戏、语音助手、无线数据桥等）。

## 硬件与开发环境（已评判 ✅ 完整）

| 项 | 状态 |
| --- | --- |
| 开发板 | 正点原子 AI BOX1（ESP32-S3-WROOM-1，带屏/麦/喇叭） |
| VS Code 扩展 | ✅ esp-idf-extension 1.9.1 + CMake Tools + C/C++ |
| ESP-IDF | ✅ **v5.2.1**（E:\ESP-IDF\Espressif\frameworks\esp-idf-v5.2.1） |
| Python | ✅ 3.11.2（idf-python 内置） |
| 烧录方式 | ✅ UART（idf.flashType=UART） |
| 串口 | COM14（ESP32S3-BOX USB 串口） |

**结论：环境已完善，可直接开发。**

## 目录结构

```
ESP/
├── README.md              # 本文件
├── docs/                  # 导入的关键资料（正点原子 AI BOX1 资料 A 盘）
│   ├── ESP32S3 BOX 开发板使用指南-IDF版_V1.2.pdf   ← 先看这个
│   ├── DNESP32S3B开发板使用指南-IDF版_V1.1.pdf
│   ├── DNESP32S3B开发板入门教程&FAQ_V1.0.pdf
│   ├── FreeRTOS开发指南_V1.10.pdf
│   ├── LVGL开发指南_V1.5.pdf / LVGL移植教程.pdf
│   ├── esp32-s3_datasheet_cn.pdf / TRM / WROOM 手册
│   └── 初学者入门必看.txt
├── notes/                 # 开发笔记
├── projects/              # ★ 工程区（每个实验/阶段一个工程，编号递增）
│   └── 01_bringup/        # 环境自检工程：芯片/Flash/PSRAM/MAC/NVS/心跳任务
│                          #   ├─ CMakeLists.txt / sdkconfig.defaults（16MB+Octal PSRAM+240MHz）
│                          #   ├─ partitions-16MiB.csv（nvs/phy/factory/vfs/storage）
│                          #   ├─ main/main.c（自检固件）
│                          #   └─ README.md（编译烧录步骤 + 期望日志）
└── .gitignore
```

## 完整资料源（本地保留，按需取用）

- 原盘：`D:\【正点原子】ESP32 AI BOX1资料（A盘）`（11.7GB）
  - `4，程序源码\v_5.2.1版本例程\`（3.1GB，**与 IDF 5.2.1 匹配**——开发前先跑通对应例程）
  - `4，程序源码\v_5.4版本例程\`（302MB）
  - `6，软件资料` / `7，硬件资料` / `8，ESP32-S3参考资料`
- 乐鑫官方：https://docs.espressif.com/projects/esp-idf/zh_CN/v5.2.1/esp32s3/

## 开发流程（第一次用，详细见 docs/ESP-IDF开发流程.md）

1. **VS Code 打开工程**：`Ctrl+Shift+P` → `ESP-IDF: Show Examples`（或打开已有工程）
2. **选择目标芯片**：`ESP-IDF: Set Espressif Device Target` → esp32s3
3. **配置**：`ESP-IDF: SDK Configuration Editor`（menuconfig）
4. **编译**：`ESP-IDF: Build your project`（或底部⚙️按钮）
5. **烧录**：`ESP-IDF: Flash your project`（自动选 COM14 + 波特率）
6. **串口监视**：`ESP-IDF: Monitor`（或底部串口图标）

## 与 MCU 交互规划（后续）

- **串口桥**：ESP32 UART ↔ MCU UART（复用 cam_link 协议风格，扩展 WiFi/BLE 数据帧）
- **WiFi**：ESP32 连网（HTTP/MQTT/WebSocket）→ 桥接 MCU 数据上云
- **蓝牙**：ESP32 BLE 与外设交互 → 桥接 MCU
- **语音**：ESP32 麦克风采集/唤醒词 → 命令 → MCU（体感游戏语音控制）
- MCU 侧预留：UART 资源已规划（见 APP/Config/pinout.h——ESP32_UART PC6/PC7 预留）

## 开发计划

- [x] **工程骨架**：`projects/01_bringup`（自建，非例程拷贝；芯片/Flash/PSRAM/MAC/NVS/心跳自检）
- [x] **编译验证**：`idf.py build` 通过（app 239.8KB / bootloader 21.1KB，88% 分区空闲）
- [ ] 烧录验证：`.\idf.ps1 -p COM14 flash monitor`，核对自检日志（Flash 16MB / PSRAM 8MB Octal / MAC / NVS）
- [ ] UART 收发验证（`04_uart` 例程）+ 与 MCU 串口桥雏形
- [ ] WiFi 例程（sta 连接）+ 与 MCU 串口桥
- [ ] BLE 例程 + 数据桥
- [ ] 语音（麦克风/唤醒词）例程
- [ ] 与 MCU 联调：WiFi/BLE/语音 → MCU 动作

## 环境备忘（踩坑记录）

> ⭐ **已封装成一键脚本**：`projects\01_bringup\idf.ps1`——自动处理下面 ①② 两项，
> 用法 `. \idf.ps1 build` / `.\idf.ps1 -p COM14 flash monitor`。

| 现象 | 原因 | 解法 |
| --- | --- | --- |
| ① `export.ps1` 报 `python_env\idf5.2_py3.12_env\... doesn't exist` + `Cannot import module "click"` | export 选中了系统 Python 3.12（`D:\Python`），而虚拟环境 `idf5.2_py3.11_env` 是用 IDF 自带 3.11.2 建的 | 调 export 前把 `E:\ESP-IDF\Espressif\tools\idf-python\3.11.2` 及其 `Scripts` 放到 PATH **最前** |
| ② `UnicodeDecodeError: 'gbk' codec can't decode byte 0x..` | 中文 Windows 下 Python 默认 GBK 编码，读 UTF-8 的 sdkconfig/构建日志即崩 | 设 `$env:PYTHONUTF8='1'`；且 **sdkconfig.defaults 保持纯 ASCII（禁止中文注释）** |
| ③ `error: 'ESP_RST_EFUSE' undeclared` 等 | 该枚举是 IDF 5.3+ 才有，5.2.1 无 | 用 `default:` 兜底，勿引用新版本 API |
| ④ `Not using an unsupported version of tool cmake found in PATH: 3.28.1` | 系统 cmake 与 IDF 不匹配 | 属提示；export.ps1 会把 IDF 自带 cmake 3.24.0 置于 PATH 前 |
| ⑤ 终端里 `idf.py` 找不到 | 未激活 IDF 环境 | 先 `. E:\ESP-IDF\Espressif\frameworks\esp-idf-v5.2.1\export.ps1`（或用 ①→`idf.ps1`） |
| ⑥ openthread 子模块报 `dubious ownership` git 警告 | git 安全目录限制 | 可忽略；或按提示 `git config --global --add safe.directory ...` |
