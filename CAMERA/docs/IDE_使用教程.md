# OpenMV IDE 使用教程 —— 实时显示摄像头图像

> 适用：OpenART mini V3.1 + OpenMV IDE 2.6.7（Windows）
> 前提：摄像头 USB 已连接电脑（会枚举出 `OpenMV Cam USB COM Port (COMx)`
> 和一个 `NXP MASS STORAGE` U 盘 —— 后者是模块里的 TF 卡）。

## 一、三步看到实时画面

### 第 1 步：打开 IDE 并连接

1. 启动 OpenMV IDE（安装目录 `D:\openmvide`，或开始菜单搜索 OpenMV IDE）
2. 观察窗口**左下角状态栏**：应显示设备名（如 `OpenMV Cam COM6` 或 `OpenART` 字样）
   - 若显示 `Disconnected`：点**工具栏的「连接」图标**（插头形状，或菜单
     `工具(Tools) → 连接(Connect)`，快捷键 `Ctrl+P`）
3. 连接成功后状态栏变为设备名，右侧「串行终端(Serial Terminal)」可看到
   摄像头返回的 print 输出

### 第 2 步：打开脚本

- 菜单 `文件(File) → 打开(Open)`，选择
  `D:\GIT-SPACE\D00\CAMERA\scripts\hello_world.py`
- 或直接在 IDE 里新建脚本（`文件 → 新建`），粘贴 hello_world.py 内容

### 第 3 步：运行

- 点**工具栏绿色「运行(Run)」按钮**（快捷键 `F5`）
- **左侧 Frame Buffer（帧缓冲）窗口**立即实时显示摄像头画面，
  左上角叠加帧率（如 `30.0 fps`）
- 停止：点红色「停止(Stop)」按钮（快捷键 `Ctrl+Shift+R` 或 `Esc`）

> 说明：`sensor.snapshot()` 抓取的每一帧都会自动推送到 IDE 左侧窗口，
> 这就是"实时显示"的原理 —— 不需要额外代码。

## 二、常用操作

| 操作 | 方法 |
| --- | --- |
| 保存当前帧为图片 | 帧缓冲窗口右键 → Save / 或菜单 `工具 → 保存快照(Save Snapshot)` |
| 录制视频 | `工具 → 开始录制(Start Recording)`（MJPG） |
| 查看串口输出 | 右侧 Serial Terminal 面板（脚本里 print 的内容） |
| 调整分辨率/帧率 | 改 `set_framesize`：QVGA(320x240) / QQVGA(160x120)；灰度比彩色快 |
| 运行板载脚本（脱离电脑） | `工具 → 将打开的脚本保存到 Cam(Save open script to Cam)`，拔线后上电自动运行 `main.py` |
| 查看固件版本 | 串口终端执行 `import os; os.uname()` 或连接时 IDE 提示 |

## 三、常见问题

1. **连不上 / 状态栏一直 Disconnected**
   - 换一根**数据线**（部分线只充电不传数据）
   - 重插 USB；确认设备管理器里有 `OpenMV Cam USB COM Port`
   - IDE 菜单 `工具 → 重置 OpenMV Cam(Reset)` 试试
2. **画面黑屏 / 无图像**
   - 等 1~2 秒（skip_frames 跳过启动帧）
   - 检查镜头是否盖住、排线是否松
3. **帧率低（<15fps）**
   - 改灰度：`sensor.set_pixformat(sensor.GRAYSCALE)`
   - 降低分辨率：`sensor.set_framesize(sensor.QQVGA)`
   - 关闭 IDE 的「显示帧率」类辅助渲染（绘制开销）
4. **TF 卡相关**：图像显示不依赖 SD 卡；但后续用 GPIO/LED/UART/SPI 时，
   需把对应固件版本的 `examples/<版本>/SD卡必备文件/`（cmm_cfg.csv + cmm_load.py）
   放到 TF 卡根目录（详见各版本「使用前必读」）
5. **镜头画面上下/左右颠倒**：在 `sensor.skip_frames` 前加
   `sensor.set_vflip(True)`（垂直翻转）和/或 `sensor.set_hmirror(True)`（水平镜像）

## 四、下一步

- 跑通实时显示后，可打开官方示例逐个尝试：
  `CAMERA/examples/V4.1.0固件示例/`（外设使用/sensor.py、save image.py 等）
- 需要做识别时参考：`外设使用/` 与 `AI模型加载&apriltag识别/` 示例
