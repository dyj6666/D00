# CAMERA —— OpenART mini V3.1 摄像头开发子空间

> 逐飞科技（Seekfree）OpenART mini V3.1 视觉模块的本地开发工作区。
> 本空间收录官方使用资料、固件示例、数据集与开发笔记；
> 后续将在此开发视觉功能，并通过**串口与 MCU（D00/APP）通讯**完善整机功能。

## 硬件概述

- 模块：逐飞科技 **OpenART mini V3.1**
- 生态：OpenMV（MicroPython）—— 用 OpenMV IDE 开发调试
- 定位：AI 视觉识别（数字/动物/水果/apriltag 等），面向智能车竞赛 AI 视觉组
- 串口：`UART2`，默认 115200，**TX=PB12 / RX=PB13**（详见 examples/V4.1.0/基础外设/UART.py）

## 目录结构

```
CAMERA/
├── README.md          # 本文件：子空间说明与索引
├── docs/              # 使用文档
│   ├── OpenART mini说明书.pdf
│   ├── 模型训练开源资料.txt
│   └── 使用前必读.txt（V3/V4.0/V4.1 三版）
├── examples/          # 官方示例（按固件版本分目录）
│   ├── V3.X.X固件示例/
│   ├── V4.0.0固件示例/
│   └── V4.1.0固件示例/   # 最新：基础外设(GPIO/LED/PWM/TIMER/UART)、
│                        #      外置外设(IPS114/TFT18/舵机)、外设使用
├── dataset/           # 数据集（数字/动物/水果/apriltag）
│   ├── 紫色边框/          # 数字0-9 + 动物 + 水果 紫色边框图
│   ├── 16届AI视觉组数据集/ # 第十六届智能车 AI 视觉组官方数据集
│   └── 动物、水果 黑色边框.rar
├── tools/             # 工具与源压缩包
│   ├── openmv-ide-windows-2.6.7.exe   # OpenMV IDE 安装包
│   ├── OpenART mini示例.7z
│   └── 数字、动物、水果 紫色边框.7z
└── notes/             # 本空间开发笔记（随开发补充）
```

## 开发环境

- **OpenMV IDE 2.6.7**：本地已装 `D:\openmvide`；安装包备份在 `tools/`
- 连接：USB 连接模块 → OpenMV IDE 识别 → 运行/烧录 `.py` 脚本
- 固件版本查看：`examples/OpenART mini示例/如何查看固件版本号.png`
- **SD 卡必备**：使用 GPIO/LED/PWM/UART/SPI 前，需把对应固件版本的
  `SD卡必备文件/`（cmm_cfg.csv + cmm_load.py）放到 SD 卡根目录（见各版本"使用前必读"）

## 串口通讯规划（与 MCU 对接）

OpenART mini ↔ MCU（D00/APP）串口链路规划（后续开发）：

- 模块侧：`UART2` 115200，TX=PB12 / RX=PB13（官方 UART.py 示例）
- MCU 侧：对应 USART 空闲端口（待定，参考 APP/Application/uart 相关服务）
- 协议：待设计 —— 建议帧格式：帧头 + 类型 + 长度 + 数据 + 校验
  （视觉结果上行：识别目标/坐标/置信度；控制指令下行：参数配置/抓拍指令）

## 官方资料链接

- NXP OpenART mini 产品页：https://aiotcloud.nxp.com.cn/details/open-art-5?lan=en
- 恩智浦AI视觉组入门教程（逐飞）：https://mp.weixin.qq.com/s/y90pT2_g0IPRuaNQPzZUqg
- 恩智浦AI视觉组浅析（逐飞）：https://mp.weixin.qq.com/s/y2IzQTzd_mr4BtPH-oOKcQ
- 模型训练开源资料（百度网盘）：https://pan.baidu.com/s/1JaIdG41KuXoVjXiAi5oMKA（提取码 eo8f）
- 恩智浦大赛 AI 视觉组培训视频：https://www.nxpic.org.cn/video/list-23/
- OpenMV 官方文档：https://docs.openmv.io/

## 开发计划（待细化）

- [ ] 熟悉 OpenMV IDE 与模块基础外设（LED/UART/GPIO/TIMER）
- [ ] 跑通 V4.1.0 示例：sensor / UART / AI 模型加载 & apriltag
- [ ] 设计并实现 OpenART ↔ MCU 串口协议（帧格式 + 收发）
- [ ] 视觉识别应用开发（目标检测/坐标输出）
- [ ] 与 D00/APP 联调（串口打通后）
