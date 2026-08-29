# D00 源码研学 · 笔记 37 —— HOST 传输层（UART / TCP / HTTP 三通道）

> 精读对象：`HOST/OTA_Tool/core/transport.py`(293)
> 阶段 4 第 2 批：与板端协议逐字节对应的上位机传输层

---

## 一、三通道帧协议对照（与板端对称）

| 通道 | 帧格式 | CRC | 确认 |
| --- | --- | --- | --- |
| UART HOSTLINK | `AA 55 cmd len payload` | CRC-16/MODBUS | 逐帧（重试 4 次） |
| TCP :9020 | `5A cmd len2BE payload` | **CRC-8（poly 0x07）** | ACK 帧（0x80）+ 流水线 |
| HTTP | 标准 HTTP GET /ota.bin | — | 分块背压 |

## 二、⭐ UartTransport（:31-109）

- `cmd(frame, expect_cmd, timeout, retries, boot_cb)`（:78-109）：
  ```
  发送一帧 → FrameParser 流式解析（自带重同步）
    → 收到 BOOT 广播(0x0C) → boot_cb（升级过程可视化）
    → 收到期望命令 → 返回
  重试 4 次（每次独立超时窗口）
  ```
- `drain()`：会话开始前清残留缓冲（重同步）

## 三、⭐ TcpTransport（:117-204）

- **TCP_NODELAY**（:134）：与板端一致禁用 Nagle
- **整帧单次 sendall**（:160-171）："避免 Nagle 拖慢（与固件端一致）"——笔记 20 的 40ms 血泪
- **流式收帧**（_recv_exact :174-188）：`_rx_buf` 累积 + 精确取 n 字节——TCP 粘包/拆包处理
- **CRC-8 校验**（:151-158 + :197）：与板端 ota_tcp_svc 的 `0x07` 多项式逐位一致
- 发送超时异常语义："对端窗口阻塞"（板端 Nagle/小窗口场景诊断）

## 四、⭐ HttpOtaServer（:212-292）

- **分块发送 8KB**（:216 + :242-247）：
  ```
  "整包一次 write 会被 Windows 发送缓冲 + 板端小窗口卡死（实测 36s 停滞），
  分块让 TCP 背压自然节流"
  ```
- 固定路径 /ota.bin + 正确 Content-Length + Connection: close
- 测试模式：无固件包时返回在线提示页（浏览器验证）
- **get_lan_ip**（:284-292）：**UDP connect 仅选路不发包**——拿到能到达板端的本地 IP（直连场景关键）
- ThreadingHTTPServer 守护线程

## 五、设计亮点

1. **协议对称**：帧格式/CRC/重试语义与板端逐字节一致（AA55/5A 两套）
2. **流式解析器**：UART FrameParser 重同步 + TCP _rx_buf 粘包处理
3. **Nagle 双端一致禁用**：40ms 血泪教训的对称实现
4. **HTTP 分块背压**：Windows 发送缓冲 + 板端小窗口卡死（36s 实测）的解法
5. **UDP connect 选路**：直连场景本地 IP 获取的巧妙技巧

## 六、待读清单（下一课）

- [x] transport（本轮完成）
- [ ] `ymodem_sender.py`(368)：YMODEM 发送器 + encrypt_and_sign + derive_aes_key_from_uid
- [ ] `hostlink.py`：帧构建/解析
- [ ] `version_lib.json`：构建号/版本管理（单一事实源）
- [ ] VLink_Debugger / LogicAnalyzer / EthLab / D00Term

## 七、自测题

1. UART 与 TCP 的帧 CRC 分别是什么？（CRC-16/MODBUS vs CRC-8 0x07）
2. UartTransport.cmd 怎么处理 BOOT 广播？（boot_cb 旁路）
3. TCP 为什么整帧单次 sendall？（Nagle 拖慢）
4. _recv_exact 解决什么？（粘包/拆包）
5. HTTP 为什么分块 8KB？（整包写卡死 36s 实测）
6. get_lan_ip 为什么用 UDP connect？（仅选路不发包）
7. 重试语义？（UART 4 次独立窗口 vs TCP 无重试抛异常）
8. 三通道的超时语义差异？（UART 轮询/TCP 精确超时/HTTP 背压）
