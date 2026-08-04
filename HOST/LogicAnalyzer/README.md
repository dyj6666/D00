# LogicAnalyzer Pro — STM32F407 8 通道逻辑分析仪上位机

配合 `APP` 固件的逻辑分析仪（TIM1+DMA2 采样引擎）使用。

## 功能

- **采集**：100kHz ~ 10MHz 采样率、IRAM/SRAM 双缓冲、触发配置、
  二进制高速下载（HOSTLINK 921600）
- **波形**：8 通道堆叠显示、缩放/平移、十字光标、区间频率/占空比测量
- **解码**：UART（波特率/奇偶/停止位）、I2C（START/地址/数据/ACK/STOP）、
  SPI（CPOL/CPHA/MOSI/MISO/CS）
- **位视图**：任意区间的每通道二进制串、十六进制、十进制、ASCII
- **导出**：原始采样 CSV、数据包 CSV

## 接线

- 控制口（COM9 @115200）：板子 USART2 调试口，用于 `la_dma_start/stop/buf/trig`
- 数据口（COM13 @921600）：板子 USART1 上位机口，用于二进制采样下载

## 运行

```bash
pip install -r requirements.txt
python main.py
```

## 测试

```bash
python -m tests.test_decoders
```
