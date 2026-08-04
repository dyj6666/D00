# VLink_Debugger

STM32F407 上位机调试器（HOSTLINK 协议）。

## 目录结构

```
vlink/     协议层（client / protocol / transport 三层分离）
model/     数据模型（buffer / variable）
ui/        界面（main_window / plot_widget / settings_panel）
utils/     配置与工具（config）
tests/     协议层主机单元测试
main.py    程序入口（PyInstaller 打包见 main.spec）
```

## 运行

```bash
pip install -r requirements.txt
python main.py
```

## 测试

```bash
python tests/test_protocol.py
```

协议层为纯 Python 实现（帧构造/解析/CRC），与 MCU 端 `SystemServices/protocol.c`
完全对齐，可跨端回归。
