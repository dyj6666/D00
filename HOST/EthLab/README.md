# EthLab - D00 以太网分析控制台

配合板载 ETH（TCP 控制台 + UDP 实时抓帧通道）的顶级以太网上位机：
深色工业风 UI，控制台、逐字节协议结构图、字段树、实时统计、离线分析与
Wireshark 导出一应俱全。

## 功能

- **TCP 控制台**（板端 9000 端口）：完整命令交互（help/info/sysmon/
  taskstats/net/led/beep/mpu/echo/stream），历史记录、快捷按钮、遥测流
  （UPTIME/HEAP/TASKS/ETH）实时显示；连接后自动执行 `net cap on` 开启抓帧。
- **实时抓包**（UDP 7778 监听）：板端把每个 TX/RX 以太网帧实时发来，
  帧列表展示 时间/方向/长度/源/目的/协议/摘要，支持协议过滤、关键字搜索
  （IP/MAC/端口/摘要/hex）、暂停、自动滚动、上限控制。
- **帧结构**：三个视图联动 ——
  - 字节结构：整帧按协议字段逐字节着色（Ctrl+滚轮缩放，悬停/点击看详情）；
  - 字段树：按层分组展示每个字段的偏移/长度/值/说明；
  - Hex：偏移 + 十六进制 + ASCII。
- **协议解析**：Ethernet II / 802.1Q VLAN / ARP / IPv4 / IPv6 / ICMP / TCP
  / UDP 字段级解码，IPv4/TCP/UDP/ICMP 校验和逐帧验算，TCP 选项解析。
- **统计**：按协议计数、TX/RX 计数、校验和错误数、截断帧数、总字节，
  实时帧率与吞吐曲线。
- **捕获管理**：保存/加载 JSON 捕获；导出标准 PCAP（Wireshark 直接打开）；
  离线粘贴 TX/RX 行或纯 hex 解析。
- **UDP 回显测试**：向板端 7777 端口发 N 包，统计成功率与 RTT
  （min/avg/max），配合抓包窗口可同时观察 ARP/IP/UDP 各层交互。

## 使用

```bash
cd D:\GIT-SPACE\D00\HOST\EthLab
pip install -r requirements.txt
python main.py
```

1. 板端上电并确认 IP（TCP 控制台 `net` 可查，`net ip 192.168.x.x` 可改）；
2. 填好板端 IP（默认 192.168.1.10:9000），点“连接”；
3. 勾选“自动抓帧”会在连接后自动执行 `net cap on`，抓帧立即开始；
4. 点“UDP 回显测试”生成流量，在“实时抓包”里逐帧查看各层结构。

## 板端抓帧通道协议（固件 v190+）

`net cap on` 后，板端把每个 TX/RX 帧封装为 UDP 发往 TCP 控制台对端 IP 的
7778 端口（源/目的端口均为 7778，IPv4 校验和=0）：

```text
载荷: dir(1) flags(1) orig_len(2, 大端) raw[]
dir   : 1=TX  2=RX
flags : bit0=1 表示截断（帧长 > 1468B，仅保留前 1468B）
orig_len: 原始以太网帧长度
```

## 板端命令

- `net cap on|off`：开/关 UDP 实时抓帧（TCP 控制台自动指向对端 IP）；
- `net dbg all|tx|rx|off`：串口逐帧十六进制打印（调试用）；
- `net`：链路/IP/MAC/收发计数/抓帧状态；
- `net ip <a.b.c.d>`：运行时改静态 IP（掉电恢复默认）。

## 目录

```text
EthLab/
  main.py              入口
  eth/decoders.py      协议解码引擎（字段级 + 校验和）
  eth/pcap.py          PCAP 导出
  ui/byte_view.py      逐字节结构图
  ui/console_panel.py  TCP 控制台
  ui/capture_panel.py  实时抓包 + 帧列表
  ui/detail_panel.py   帧详情（结构图/字段树/Hex）
  ui/stats_panel.py    统计 + 速率曲线
  ui/echo_dialog.py    UDP 回显测试
  ui/main_window.py    主窗口
  tests/smoke_test.py  离线冒烟测试
```
