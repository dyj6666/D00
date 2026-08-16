# CAMERA 开发笔记

> 随开发补充；本文件入库（CAMERA/.gitignore 白名单）。

## 2026-08-16 MCU 端摄像头链路（已完成 ✅ 用户确认完全正常）

- **硬件**：OpenART mini B12/B13(UART2) ↔ MCU UART5 PC12/PD2 @115200 交叉互联
- **MCU 接收**：顶级 IDLE+DMA——DMA1_Stream0/CH4 循环搬入 256B SRAM 环形缓冲，
  UART5 IDLE 中断每帧一次消费（零字节中断、零丢帧），cam_link 状态机解析
  AA55 帧协议（0x01 坐标 / 0x02 挥手 / 0x03 手势）
- **⚠ 经典坑**：DMA 缓冲不能放 CCM（0x10000000 仅 CPU 可访问）——放 CCM 时
  DMA 写入无效且总线错误致 LCD 卡死（实测复现+修复）
- **GUI**：新增 CAM 状态页（帧/错误/挥手/手部/手势 250ms 实时）+ 4 按钮导航
- **交互**：挥手换页+蜂鸣提示；KEY0 不再切 LED
- **调试**：`cam` 命令（统计 + DMA ndtr/cr/缓冲头诊断）
- **版本**：build 9173（基础链路）→ 9177（IDLE+DMA）→ 9180（SRAM 缓冲修复）
- **遗留**：OpenART 侧 UART 输出依赖其 UART2 初始化成功（IDE 运行 gesture_ai.py
  时发送）；body 后续体感游戏基于 0x01 坐标帧 + 0x03 手势帧

## 2026-08-16 挥手翻页（已完成 ✅）

- **双模式互斥**：手静止 ≥600ms（帧间位移<12px）→ 手势识别模式（只跑 AI）；
  位移 ≥20px/帧 → 立即切挥手检测模式（只跑挥动判定）——两功能互不干扰
- 挥手检测：窗口范围法（6 帧历史，范围 ≥110px 触发，方向=窗口净位移
  横向取反补偿镜像）——对跟踪丢帧鲁棒，快速挥必检到
- 协议：0x02 SWIPE_LEFT/RIGHT → MCU `GuiPages_PageNext()`（翻页）
- 运动模式跳过 classify → 帧率提升；模式切换打印 [MODE] 提示

## 2026-08-16 手势识别系统（已完成 ✅ 用户确认基本零失误）

- **方案**：LAB 肤色检测（黑色桌面场景，放宽形状过滤 0.4-2.5/MAX 320）
  + 端部定位裁剪（质心外推 0.6 + 边长限 150px 聚焦手）
  + tflite CNN 分类（32x32，6 类：fist/ok/one/palm/two/victory）
  + 7 帧多数投票 + 3 帧切换确认（误切概率 ~1.4%）
- **模型**：train/dataset（360 张多样化采集）+ train_gesture.py 离线增强
  （旋转±25°去黑边/平移±15%/缩放0.75-1.3/翻转/亮度）80 epochs
  + 调度+早停；单帧 val~76-80%，部署投票后实际基本无误判
- **协议**：0x01 坐标帧 + 0x03 AI 手势帧（30Hz，见 串口协议.md）
- **易混类**：one/two、palm/victory（视觉相似，靠采集姿势差异化缓解）
- **关键脚本**：collect_gestures.py / train_gesture.py / gesture_ai.py
- **训练环境**：uv venv Python 3.12.13 + tensorflow 2.21（CAMERA/train/.venv）
- **遗留**：UART 输出需匹配 V4.3 固件的 cmm 文件才能真正从 B12/B13 发出
  （当前 print 验证通路；MCU 对接时向逐飞要 cmm 或改用 debug UART 方案）

## 2026-08-16 人脸检测排查结论（重要）

- **OpenART mini V4.3 固件的 Haar 人脸检测失效**：
  - 级联加载正常（frontalface 25 级 / 2913 特征 / 6383 矩形，数据完整）
  - 但实时画面（含 2x 放大预处理）与离线标准正脸 BMP（320x240 灰度，
    多阈值 0.75~0.2）**全部 0 检出** → 逐飞定制固件的 Haar 检测引擎移植缺陷
    （OpenMV 标准 STM32 板上该功能正常），用户侧无法修复，需固件升级才能用
  - OpenMV 图像可变性（mutable）规则实测：`find_features` 只接受帧缓冲图像；
    文件加载的图（JPEG/BMP 均）只读，需 `sensor.snapshot()` 后 `draw_image` 画入
  - 文件加载：JPEG 在 OpenART 上有兼容问题（大图报 too big、draw 报错），
    **BMP 最可靠**（320x240 灰度 BMP 78KB 加载正常）
- **替代方案（已跑通✅）**：LAB 肤色检测 `find_blobs`（V4.3 核心功能可靠）——
  `scripts/skin_detect.py`；实测 QVGA RGB565 下**稳定检出人脸区域**（30fps，
  x≈52 y≈56 w≈63 h≈81，位置/尺寸帧间稳定）
  - 实测肤色区域 LAB 统计：L[23-86] A[-7-27] B[-10-29]（含头发/阴影混合）
  - 当前阈值 (40,90, 5,30, 10,45) + 面积/宽高比过滤（>40px、0.5~1.8）可用，
    后续按场景可收紧阈值减少背景误检

## 2026-08-16 硬件与环境确认

- **固件版本**：MicroPython v1.18 / OpenMV **V4.3** / HAL v1.1.0 / BOARD: OpenART mini-MIMXRT1060
  （IDE 运行任意脚本时串行终端首行可见；后续配 cmm_cfg.csv 以此版本为准）
- **OpenMV IDE 2.6.7**：连接时弹"固件已过时"→ **点取消跳过**即可正常使用；
  点确认会进升级流程并报"不支持的主板架构"（RT1064 非标准 OpenMV 主板，IDE 无法升级）
- **TF 卡必备文件（cmm_cfg.csv / cmm_load.py）**：
  - 官方要求：使用 GPIO/LED/PWM/UART/SPI 前需放到 SD 卡根目录
  - ⚠️ **版本必须与固件匹配**：实测 V4.1.0 版 csv 在 V4.3 固件上报
    `pinOBJ IS null error in cmm_cfg.cvs`（引脚命名不识别），且**固件启动自动加载，
    报错会中断所有脚本运行**（连 sensor 拍照都跑不了）——不匹配时**先删掉 csv** 恢复
  - 需要外设时再找匹配 V4.3 的 csv（资料自带三版：V3.X.X / V4.0.0 / V4.1.0，均待验证）
- **图像存 TF 卡取回流程**：
  1. IDE 运行 `scripts/capture_once.py`（`img.save("/sd/photo.jpg")`）
  2. 串行终端出现 `saved /sd/photo.jpg OK`
  3. **IDE 连接期间模块 U 盘（MSC）禁用**——需断开 IDE 连接或重插 Type-C，
     G: 盘出现后复制 `G:\photo.jpg`
- **照片取证**：`notes/photo_capture.jpg`（320x240 QVGA，19.5KB JPEG，有效完整）

## 待办

- [ ] 确认 V4.3 固件配套的 cmm_cfg.csv（联系逐飞官方渠道或对比三版测试）
- [ ] 研究 OpenMV USB 协议实时抓帧（IDE 能连说明协议兼容，可逆向帧传输命令）
- [ ] 串口协议设计（UART2 115200 TX=B12 RX=B13）与 MCU 对接
