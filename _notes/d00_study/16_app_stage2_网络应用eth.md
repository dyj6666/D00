# D00 源码研学 · 笔记 16 —— 网络应用层（eth_app）

> 精读对象：`Application/eth_app.c`(785)
> 阶段 2 应用层第 1 批：以太网状态聚合 + 抓帧 + ICMP/UDP 工具——**全部用 lwIP raw API**

---

## 一、模块定位

**依赖 LwIP 的 raw API（非 socket/Netconn）**：raw PCB 直接在 tcpip 线程上下文收发，无阻塞拷贝——适合嵌入式轻量场景。`netif gnetif` 来自 LWIP/App/lwip.c（CubeMX 生成适配层）。

## 二、功能全景

| 功能 | API | 说明 |
| --- | --- | --- |
| DHCP 动态/静态回退 | dhcp_start/stop + 一次性定时器 | **超时回退静态 IP**（默认 192.168.1.10/24 或 EEPROM 保存值） |
| 实时抓帧（EthLab） | raw UDP :7778 | TX/RX 帧原样转发到上位机（3 槽流水线） |
| ICMP ping | raw ICMP + 信号量 | shell 上下文阻塞等待 RTT |
| UDP 诊断发送 | raw UDP（checksum=0） | `net udp <ip> <port> <hex>` |
| UDP 回显服务 | raw UDP :7777 | RTT/吞吐验证 |
| 运行时改 IP | tcpip_callback + netif_set_addr | **tcpip 线程安全** |
| 状态聚合 | 计数 + RefreshStatus | LCD/sysmon/shell 1s 同步 |

## 三、⭐ 关键设计

### 1. DHCP 超时回退（:49-100）
```
EthApp_DhcpStart → tcpip_callback(dhcp_start) + osTimerNew(一次性, ETH_DHCP_FALLBACK_MS)
回调：未 BOUND → dhcp_stop → 应用静态 IP（默认或 EEPROM 保存值）
```
**DHCP 状态机读取**（EthApp_DhcpState :121-136）：BOUND/REQUESTING/SELECTING/REBOOTING/INIT/BACKING_OFF → 可读字符串。

### 2. ⭐ 实时抓帧通道（EthLab UDP :7778，:138-332）——流水线设计
```
TX/RX 帧（lwip 钩子）→ eth_cap_fill（临界区取槽，3 槽循环）
  → 拷贝帧数据到槽 → tcpip_callback(eth_cap_send_cb) → tcpip 线程组 UDP 包发送 → 归还槽
```
- **MTU 防分片**：`ETH_CAP_MAX=1468`（MTU1500-IP20-UDP8-帧头4）
- **防自抓环**（s_cap_sending）：发送抓帧包期间忽略 TX 钩子——否则抓帧包又被抓，无限递归
- **槽耗尽丢弃计数**（s_cap_drop）：满 3 槽在途即丢
- **16bit 源/目的端口相同的伪 UDP 头**（:171-179）：raw API 手拼 UDP 头，checksum=0（IPv4 合法）

### 3. ⭐ ICMP ping（raw API + 信号量，:334-440）
- **手拼 ICMP echo 包**：type/code/chksum/id/seq + 32B 0xAA 填充
- **应答匹配**：ping_recv 回调校验 id+seq（大端字节序显式比较）
- **阻塞等待**：`osSemaphoreAcquire(sem, 50ms)` 循环 + 总超时——shell 上下文执行（注释：阻塞执行于 shellTask）
- **raw 回调纪律**（:357-359）：payload 仍在 IP 头处，**只读偏移不改动 pbuf**——否则未吃包时触发 lwIP 断言 "altered pbuf payload pointer"

### 4. UDP 回显（:602-646）
- raw 回调里**显式字节比较目的端口**（与字节序无关）→ 整包拷贝 → 交换源/目的端口回显
- 非回显端口：return 0 放行给协议栈

### 5. tcpip 线程安全（:442-546）
- **所有 netif 操作经 tcpip_callback 投递**到 tcpip 线程执行——外部任务绝不直接调 netif API
- 静态参数块 `s_net_addr`（单实例，串行调用安全）
- 改 IP 持久化：EEPROM 保存（net_config），上电 tcpip_callback 应用

### 6. 状态聚合（:650-755）
- 链路中断回调只做**轻量计数**（EthApp_CountRx/Tx、SetLinkState）
- `EthApp_RefreshStatus` 聚合：link/IP/GW/MAC/包计数/链路时长（uptime）
- 注册 sysmon 监控项 + 3 个 HOSTLINK 变量（eth_link/rx/tx）

## 四、设计亮点

1. **raw API 全栈**：无 socket 开销，tcpip 线程内零拷贝处理
2. **抓帧流水线**：3 槽 + tcpip_callback 投递——ISR/钩子上下文只拷贝不发送
3. **防自抓环**：发送期间忽略 TX 钩子——递归灾难的预防
4. **tcpip_callback 纪律**：netif 操作全部投递到 tcpip 线程
5. **DHCP 回退**：动态失败自动回静态——嵌入式网络鲁棒性
6. **字节序显式**：所有多字节字段显式按字节比较/组装——跨平台安全

## 五、待读清单（下一课）

- [x] eth_app（本轮完成）
- [ ] `tcp_svc.c`：TCP 控制台服务（端口 9000，命令会话）
- [ ] `icmp_svc.c`：ICMP 应答服务（限速）
- [ ] `dns_svc.c` / `sntp_svc.c` / `mqtt_svc.c` / `http_svc.c`
- [ ] `ota_tcp_svc.c` / `ota_http_svc.c` / `ota_can_svc.c`

## 六、自测题

1. 抓帧通道为什么用 3 槽 + tcpip_callback？直接在钩子函数里 raw_sendto 会怎样？
2. 防自抓环标志解决什么？没有它会怎样？（无限递归）
3. ping 的 raw 回调为什么不能改动 pbuf payload 指针？
4. 为什么所有 netif 操作都要经 tcpip_callback？直接调会怎样？
5. DHCP 超时回退的静态 IP 从哪来？（两级：默认/EEPROM）
6. UDP 手拼头的 checksum 为什么可以填 0？（IPv4 合法）
7. ETH_CAP_MAX=1468 怎么算的？为什么必须小于 1472？
8. udp_echo_recv 怎么判断"这是发给我的回显包"？（端口字节比较）
