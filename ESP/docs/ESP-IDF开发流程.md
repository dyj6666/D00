# ESP-IDF 开发流程（第一次使用指南）

> 环境：VS Code + ESP-IDF 扩展 + ESP-IDF v5.2.1（E:\ESP-IDF）
> 板卡：正点原子 AI BOX1（ESP32-S3），串口 COM14

## 一、核心概念（先理解再动手）

| 概念 | 说明 |
| --- | --- |
| **ESP-IDF** | 乐鑫官方开发框架（基于 CMake + FreeRTOS 的 C/C++ 工程体系） |
| **idf.py** | 命令行工具（构建/烧录/监视/配置）——VS Code 扩展已封装成按钮 |
| **menuconfig** | 图形化配置（Kconfig）——芯片/外设/协议栈开关 |
| **target** | 目标芯片（esp32s3）——决定编译哪个架构 |
| **components** | 工程组件目录（自定义代码按组件组织） |
| **sdkconfig** | 配置产物（menuconfig 保存，生成于工程根） |

## 二、工程结构（ESP-IDF 标准）

```
my_project/
├── CMakeLists.txt          # 顶层：指定组件/依赖
├── main/
│   ├── CMakeLists.txt      # 主组件
│   └── main.c              # 入口 app_main()
├── components/             # （可选）自定义组件
├── sdkconfig               # menuconfig 产物（自动生成）
└── build/                  # 构建产物（自动生成）
```

入口函数：`void app_main(void)`（替代裸机 main，由 FreeRTOS 调度）

## 三、完整开发循环（VS Code 操作）

### 1. 创建/打开工程
- **新工程**：`Ctrl+Shift+P` → `ESP-IDF: Show Examples` → 选例程（Create project）
  或 `ESP-IDF: Create New Project`
- **打开已有**：`文件 → 打开文件夹`（工程根目录，含 CMakeLists.txt）

### 2. 首次配置
1. `Ctrl+Shift+P` → `ESP-IDF: Set Espressif Device Target` → **esp32s3**
2. `Ctrl+Shift+P` → `ESP-IDF: SDK Configuration Editor` → 需要时改配置
   （常用：Serial flasher config → 波特率 921600/115200；Partition table）

### 3. 编译
- 底部状态栏 **⚙️（Build）** 按钮，或 `Ctrl+Shift+P` → `ESP-IDF: Build your project`
- 输出：`build/xxx.bin` + 烧录地址

### 4. 烧录
- **🔌（Flash）** 按钮 → 自动选端口（COM14）→ 下载 → 复位运行
- 首次烧录需按住 BOOT 键（如遇连接失败）

### 5. 监视串口
- **📡（Monitor）** 按钮 → 打开串口监视（log 输出 / 输入命令）
- `Ctrl+]` 退出监视

### 6. 迭代循环
```
改代码 → Build → Flash → Monitor（看日志）→ 改代码...
```
> 快捷：底部状态栏三个按钮一键完成。

## 四、最小示例（Hello World）

```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "hello";

void app_main(void)
{
    ESP_LOGI(TAG, "Hello from ESP32-S3!");
    for (int i = 0; i < 10; i++) {
        ESP_LOGI(TAG, "count %d", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

## 五、常用命令（命令行方式，VS Code 终端）

```powershell
# 进入 IDF 环境（每次新终端）
E:\ESP-IDF\Espressif\idf_cmd_init.bat
# 或（VS Code 扩展已内置环境，直接点按钮即可）

# 在工程目录：
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM14 flash
idf.py -p COM14 monitor
```

## 六、日志与调试

- **日志分级**：`ESP_LOGE/ESP_LOGW/ESP_LOGI/ESP_LOGD/ESP_LOGV`
- **组件过滤**：`idf.py -p COM14 monitor` 里输入 `filter` 可过滤标签
- **崩溃回溯**：monitor 会自动输出 panic backtrace（`addr2line` 解码）

## 七、常见问题

| 问题 | 处理 |
| --- | --- |
| 烧录失败（连接不上） | 按住 **BOOT 键**再点 Flash；换 USB 线/口；检查 COM 号 |
| 编译报 Python 错误 | 确认 VS Code 扩展的 pythonInstallPath 配置（已配 E:\ESP-IDF） |
| monitor 乱码 | 波特率不匹配（看 sdkconfig 的 flash/console 波特率） |
| 目标芯片错 | `Set Espressif Device Target` 选 esp32s3 |
| 第一次编译慢 | 正常（全量编译 5-15 分钟）；后续增量快 |

## 八、正点原子 AI BOX1 注意点（据使用指南）

- 板载 **2.4 寸屏 + 麦克风 + 喇叭**（语音/显示例程见 `4，程序源码\v_5.2.1版本例程\`）
- 出厂有**小智 AI 固件**（语音助手）；开发例程从 `程序源码` 选同版本
- 例程与 IDF 版本严格匹配：**用 v5.2.1 例程 + IDF 5.2.1**
- 首次上电/烧录前看 `1，入门资料\初学者入门必看.txt`

## 九、下一步（与 MCU 联动规划）

1. 跑通 hello_world + 串口输出 ✅（验证环境）
2. WiFi STA 例程（连接路由器/或与 MCU 组网）
3. UART 桥接：ESP32 UART ↔ MCU（复用 cam_link 协议扩展）
4. BLE / 语音例程 → 桥接 MCU
