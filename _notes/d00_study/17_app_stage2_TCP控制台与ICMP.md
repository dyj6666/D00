# D00 源码研学 · 笔记 17 —— TCP 控制台与 ICMP 服务

> 精读对象：`Application/tcp_svc.c`(256) · `icmp_svc.c`(202)
> 阶段 2 应用层第 2 批：网络命令通道（端口 9000）+ ICMP 应答服务

---

## 一、⭐ TCP 控制台（tcp_svc.c）—— 命令框架的 TCP 传输适配器

### 1.1 架构
```
tcp_server_task（TcpSvc 任务，1024B 栈）
  → netconn 监听 :9000（backlog 2）
  → accept → 每客户端建 TcpCli 任务（2048B 栈，Normal）
      → tcp_client_task：
          · tcp_nagle_disable（★ Nagle+延迟ACK 会拖慢/阻塞大输出，实测 help/info 超时断连）
          · 取对端 IP（netconn_peer）
          · Cmd_SessionReset(sess, CMD_TRANSPORT_TCP, &cli, tcp_ctx_out)
          · 循环：netconn_recv_tcp_pbuf → 64B 分块 → Cmd_SessionFeed 分发
          · 输出：LOG_Printf → cmd_log_sink → ctx->out → tcp_ctx_out → netconn_write
```

### 1.2 关键设计
1. **命令框架零感知**：注册 `cmd_transport_t{TCP, CMD_TRANSPORT_TCP}`——所有 52 条命令自动可用（除仅 UART 的）；命令内用 `Cmd_ActiveUser()` 取客户端（如 `net cap on` 自动取 TCP 对端作 EthLab 目标）
2. **栈 2048 血泪注释**（:31-33）：命令处理（LOG_Printf + netconn_write）栈深大于 1024B，**实测 help/info 触发栈溢出连接被重置（Crash seq 递增）**
3. **Nagle 禁用**：命令输出为多段小写，Nagle+延迟 ACK 拖慢/阻塞大输出（与 OTA-TCP 一致）
4. **会话超时双模式**（:185-186）：stream_on 时 1s 超时（推遥测），否则 120s 空闲断连
5. **遥测流**（stream on，:115-127）：每秒推 `UPTIME/HEAP/TASKS/ETH 状态` + 提示符
6. **并发上限 2**：超限 accept 后立即 close + rejected 计数
7. **客户端计数**（s_stat.clients--）：任务退出时递减

### 1.3 与 UART Shell 的对比
| 维度 | UART Shell | TCP 控制台 |
| --- | --- | --- |
| 行编辑 | 全功能（历史/补全/方向键） | **无**（Cmd_SessionFeed 纯行分发） |
| 输出路由 | shell_uart_out | tcp_ctx_out（netconn_write） |
| 空闲超时 | 无 | 120s / 1s（stream） |
| 并发 | 1 | 2 |

## 二、ICMP 服务（icmp_svc.c）—— raw PCB 接管 echo

### 2.1 核心流程
```
raw PCB 收 ICMP → icmp_svc_recv（tcpip 线程上下文）
  → 校验 ICMP echo 头（pbuf_copy_partial，只读不改）
  → 1s 滑动限速窗口（500 pps 默认）
  → 超限/静默：吞包不计回（return 1 消费掉）
  → 自组 reply：拷贝 ICMP 部分 → type=0 → 重算校验和 → raw_sendto
  → RTT 统计（DWT CYCCNT 精确 us）
```

### 2.2 关键设计
1. **限速防放大攻击**：`icmp_rate_tick` 1s 窗口计数，超限吞包——ICMP 反射放大防护
2. **静默模式**（enabled=0）：`icmp reply off`——设备"隐身"（ping 不通）
3. **DWT 精确 RTT**（:86,114）：CYCCNT 差 / 168 = µs——软件处理时延（非网络 RTT，仅"设备响应耗时"）
4. **raw 回调纪律**（:58）：payload 在 IP 头处，只读偏移不改动 pbuf
5. **统计仪表**：echo rx/tx/drop、rate/peak pps、min/avg/max RTT、last peer/seq——sysmon 监控项注册
6. **放行语义**：非 echo（差错/应答）return 0 放行给 lwIP 默认处理

## 三、设计亮点

1. **传输适配器模式**：UART/TCP 同一命令目录，新传输 = 注册表一行
2. **Nagle 禁用**：交互协议的大输出场景必备
3. **限速+静默**：ICMP 服务的攻防两面
4. **会话超时自适应**：stream 模式 1s / 普通 120s
5. **客户端栈 2048**：命令输出路径栈深实测驱动

## 四、待读清单（下一课）

- [x] tcp_svc / icmp_svc（本轮完成）
- [ ] `dns_svc.c` / `sntp_svc.c` / `mqtt_svc.c` / `http_svc.c`
- [ ] `ota_tcp_svc.c` / `ota_http_svc.c` / `ota_can_svc.c`
- [ ] `gui_app.c` / `gui_pages.c`(1876) / `gui_theme.c`：LVGL GUI
- [ ] `data_agent.c` / `buzzer_app.c` / `key_app.c` / `led_app.c`

## 五、自测题

1. TCP 客户端执行 help 为什么曾栈溢出？（血泪注释）为什么 2048 才安全？
2. Nagle 为什么必须禁用？命令输出的特点是什么？
3. TCP 控制台为什么没有行编辑？UART Shell 和 TCP 会话的差异？
4. stream on 时接收超时为什么变 1s？（提示：遥测推送）
5. ICMP 限速防什么攻击？静默模式怎么理解？
6. RTT 统计的是网络 RTT 还是设备响应耗时？怎么测的？
7. raw 回调里为什么只读偏移不改动 pbuf？（提示：lwIP 断言）
8. 非 echo 的 ICMP 包为什么 return 0？（放行语义）
