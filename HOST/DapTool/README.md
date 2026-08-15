# D00 DAP 调试台

顶级 CMSIS-DAP（KEIL DAP）可视化上位机，把 DAP 的全部能力集成到
一个轻量界面里，日常烧录/升级/调试不再敲命令行。

## 启动

```powershell
python HOST\DapTool\dap_gui.py
```

依赖：Python 3.8+、PyQt5（`pip install PyQt5`）、OpenOCD
（`tools/xpack-openocd-0.12.0-7`，仓库内置）。

## 功能总览

| 页签 | 功能 |
| --- | --- |
| 连接 | 一键连接/断开；自动识别芯片（STM32F407）、内核、Flash 容量、DPIDR；失败自动 USB 重枚举重试 |
| 烧录 | 任意 .bin 任意地址；预设 BOOT/RUN/DOWNLOAD/PARAM；**BOOT+APP 一键**；500kHz + 分块 + IWDG 窗口 + 整包校验（实测 243KB 连续 4 轮零假死） |
| OTA 助手 | 全 DAP 驱动升级：预置加密包 → 写 BKP 升级标志(0x5A5A) → 复位 → BOOT 自动应用；完全不需要串口 |
| 调试 | 断点（硬件）/清除/列表、暂停/继续/单步、反汇编、**故障诊断**（CFSR/HFSR/DFSR/BFAR 解码 + RCC 复位原因） |
| RTOS 任务 | **FreeRTOS 内核感知**：遍历就绪/延时链表显示任务名/优先级/状态/栈已用/TCB，符号自动解析 APP.map |
| 内存 | 任意地址读/写 32 位字，Hex 视图 |
| 寄存器 | 核心寄存器 + 设备 UID + 固件信息（魔数/版本/最后构建）+ **板载崩溃记录解码**（BKP 'ERR1'） |
| 目标控制 | Halt / Resume / Reset；"持续暂停"自动喂 IWDG |
| 工具 | 扇区擦除、Flash 转储、文件比对 |

## 世界级亮点

- **崩溃记录直读**：APP 崩溃摘要存在 RTC 备份寄存器（'ERR1'），一键
  读出原因/PC/LR/CFSR-HFSR 位解码/任务名/运行时长，零串口；
- **RTOS 内核感知**：遍历 FreeRTOS 就绪/延时链表，按优先级展示任务
  与实时栈水位（0xA5 填充扫描），符号地址随构建自动更新；
- **故障诊断**：SCB CFSR/HFSR/DFSR/BFAR/MMFAR 逐位解码 + 复位原因
  （RCC_CSR）解析；
- **全 DAP OTA**：预置包 → 升级标志 → 复位，整条升级链零串口。

## 关键细节：RUN 区直烧必须带魔数

BOOT 只在 0x080DFFF8 处校验 APP 有效性魔数（0x4F54412E）。直接烧录
原始 APP.bin 到 0x08010000 时魔数会落在错误偏移，BOOT 会判定 APP
无效。因此：
- "烧录"页目标地址选 RUN(0x08010000) 时，会自动把 APP.bin 补成
  832KB 完整分区镜像（魔数@0x0CFFF8、版本@0x0CFFFC，版本取自
  config/version.json）再烧录；
- "BOOT+APP 一键"内部同样自动处理，刷完即为可启动固件。

## 关键设计（可靠性）

- 烧录固定 500kHz SWD，规避 DAP HID 假死（2MHz 长写入约 20s 必死）；
- 烧录前经调试口把 IWDG 预分频临时改为 256（约 22s 窗口），整次
  擦除+写入+校验不被目标看门狗复位打断；结束后复位由 BOOT 恢复原配置；
- 按 F407 真实扇区映射对齐擦除（s0-s3=16KB、s4=64KB、s5-s11=128KB）；
- 所有读取操作"暂停-读-恢复"毫秒级完成，不打扰正在运行的应用。

## 注意

- 烧录 BOOT 会先擦除引导区，中途断电需重新烧录才能恢复引导；
- 连接后目标保持运行；"目标控制"里的暂停才是真正的暂停；
- 关闭窗口自动断开 OpenOCD 会话。
