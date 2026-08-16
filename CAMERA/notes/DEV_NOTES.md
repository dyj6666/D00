# CAMERA 开发笔记

> 随开发补充；本文件入库（CAMERA/.gitignore 白名单）。

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
