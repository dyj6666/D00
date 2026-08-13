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
| 连接 | 一键连接/断开；自动识别芯片（STM32F407）、内核、Flash 容量、DPIDR |
| 烧录 | 任意 .bin 烧录到任意地址；预设 BOOT/RUN/DOWNLOAD/PARAM；500kHz + 分块 + IWDG 窗口扩展 + 整包校验（实测 243KB 连续 4 轮零假死） |
| OTA 助手 | 全 DAP 驱动升级：预置加密包 → 写 BKP 升级标志(0x5A5A) → 复位 → BOOT 自动应用；完全不需要串口 |
| 内存 | 任意地址读/写 32 位字，Hex 视图 |
| 寄存器 | 核心寄存器（r0-r15/sp/lr/pc/xpsr）+ RTC 备份寄存器（BKP0R-3R） |
| 目标控制 | Halt / Resume / Reset；"持续暂停"自动喂 IWDG 防看门狗复位 |
| 工具 | 扇区擦除、Flash 转储到文件、文件与 Flash 比对 |

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
