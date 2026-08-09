# D00 命令行终端（D00Term）

与固件 `cmd_transport_t` 对称的**可插拔传输**配套终端：界面只有端口选择与
命令行，无任何多余控件；UART / ETH 现用，CAN 预留扩展。

## 启动

- 双击 `start_term.bat`（自动用 `D:\Python\python.exe` 或 PATH 中的 python 启动）；
- 或命令行直接运行：

```powershell
python d00term.py                    # 交互选择传输
python d00term.py com9               # UART 默认 115200
python d00term.py tcp                # ETH 自动探测"电脑同网段"设备 IP
python d00term.py tcp 192.168.1.10   # 或显式指定
python d00term.py com9 -x "ver"      # 单次执行（脚本化）
```

ETH 无参连接时会自动枚举电脑物理网卡（过滤 VMware/ICS/APIPA），依次探测
出厂 IP（192.168.1.10）与各网段 `.10`；配合固件 `net ip` 持久化，只需在
UART 端设置一次：

```text
python d00term.py com9 -x "net ip 192.168.10.10"   # 保存到 flash
python d00term.py tcp                                # 之后每次直接连
```

`net ip default` 可清除保存配置并恢复出厂 192.168.1.10。

## 会话模式

| 传输 | 模式 | 说明 |
| --- | --- | --- |
| UART | 原始透传 | 按键原样转发，设备 shell 自带回显/历史/Tab 补全 |
| ETH | 行编辑会话 | 本地回显 + 上下键历史，设备持有 `D00> ` 提示符 |
| CAN | 扩展点 | 对应固件 `cmd_can.c`，接入后补一个类 + 一行注册 |

## 常用命令

```text
help       命令列表（含各命令可用终端标注）
info       系统信息
ver        固件版本
net        网络状态
taskstats  任务与栈水位
echo xx    连通性测试
```

## 扩展新传输（如 CAN）

继承 `Transport` 实现 `open/send/recv/close`，在 `TRANSPORTS` 注册表加一行，
会话与命令逻辑零改动——与固件侧架构完全对称。
