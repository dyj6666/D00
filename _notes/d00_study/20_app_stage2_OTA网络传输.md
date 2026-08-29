# D00 源码研学 · 笔记 20 —— OTA 网络传输：TCP 服务端 + HTTP 客户端

> 精读对象：`Application/ota_tcp_svc.c`(277) · `ota_http_svc.c`(255)
> 阶段 2 应用层第 5 批：两种以太网 OTA 通道——**服务器模型 vs 客户端拉取模型**

---

## 一、⭐ OTA-TCP（ota_tcp_svc.c）—— 设备作 TCP 服务器（:9020）

### 1.1 帧协议（自定义二进制）
```
[magic 0x5A][cmd][len u16 BE][payload][CRC-8（cmd+len+payload，poly 0x07）]
cmd: 0x01 BEGIN(ver+size) / 0x02 DATA(off+240B) / 0x03 END / 0x04 STATUS / 0x05 RESET
ACK: 0x80 命令帧（状态码 / 状态+rx+total 9B）
```

### 1.2 三个血泪级设计
**1. 整帧一次性写入**（:49-66）：多次小写触发 Nagle/延迟 ACK 交互，**实测每个 ACK 被拖慢 ~40ms（:9000 单次写入仅 1.7ms）**——tcp_send 把整帧组好一次 netconn_write。

**2. Nagle 禁用**（:243-246）：ACK 帧立即发送，避免流水线突发时小段 ACK 在发送队列堆积（TCP_SND_QUEUELEN 满）导致 netconn_write **永久阻塞、OTA 服务挂死**（实测 window>=16 复现）。

**3. 流式逐帧处理**（:151-208）：遍历 pbuf 链（`netbuf_first/next`），段内数据灌入缓冲，**凑够一帧立即处理**——若一次性只拷贝 249B 而丢弃段内剩余帧，流水线多帧同段到达时帧错位（**实测 DATA 状态 2**）。失步/超长帧整帧重同步。

### 1.3 任务优先级血泪（:272-274）
与 TCP 控制台/HTTP 同优先级（Normal）——**低优先级导致每请求唤醒延迟 ~40ms**（实测 :9000 Normal=1.6ms vs :9020 Below=43ms）。栈 2048 血泪（:268-271）：GCC 下 Ota_Begin（擦除+会话保存）+netconn 路径栈深超 Keil 实测 504B；1024B 导致 BEGIN 处理后连接异常断开。

## 二、⭐ OTA-HTTP（ota_http_svc.c）—— 设备作 HTTP 客户端拉取

### 2.1 流程（客户端拉取模型）
```
`ota http <ip[:port]>/<path>`（shell 上下文阻塞执行）
  → GET path HTTP/1.0 → 解析响应头（逐行找 Content-Length）
  → 校验（0 < len ≤ OTA_EXT_DL_SAFE）
  → 攒满包首部 12B → 解析版本号（小端，偏移 4..7，cryptor.py '<III12sII'）
  → Ota_Reset()（★ HTTP 拉取总是从 0 顺序写：清残留会话，避免命中同版本
       断点续传导致 offset 不一致——TCP 服务器路径保留续传能力走 STATUS 查询）
  → Ota_Begin → 流式 Ota_Data（240B 块）→ Ota_End（复位进 BOOT）
  → 进度每 48KB 打日志（防刷屏）；失败 Buzzer_OtaFail 三短音
```

### 2.2 健壮性设计
1. **超时重试 6 次**（:165-173）：中途收包超时（偶发丢包+TCP 重传延迟）连接仍可能存活——连续 6 次无数据才判定失败，避免长时间阻塞 shell
2. **头尾进位处理**（:192-237）：HTTP 头与 body 可能在同一 pbuf 段——先解析头（找 `\r\n\r\n`），段内剩余字节即 body；跨段继续
3. **pbuf 链遍历**（:182-237）：大响应跨 pbuf 边界完整读取
4. **头部行解析**（http_hdr_key_eq :43-54）：大小写不敏感 key 匹配

## 三、设计亮点

1. **两种模型互补**：TCP 服务器（上位机主动推，支持断点续传）/ HTTP 客户端（设备主动拉，简单可靠）
2. **CRC-8 帧校验**：轻量防误帧（与 HOSTLINK CRC-16 各司其职）
3. **共核复用**：BEGIN/DATA/END 全部走 ota_agent——三通道（UART/TCP/HTTP）同一下载核心
4. **防帧错位**：流式逐帧 + 段内剩余帧不丢弃
5. **防续传误命中**：HTTP 路径先 Ota_Reset（拉取总是从头写）

## 四、待读清单（下一课）

- [x] ota_tcp_svc / ota_http_svc（本轮完成）
- [ ] `ota_can_svc.c`(241)：CAN OTA 通道
- [ ] `gui_app.c`(351) / `gui_pages.c`(1876) / `gui_theme.c`：LVGL GUI
- [ ] `data_agent.c` / `buzzer_app.c` / `key_app.c` / `led_app.c` / `cmd_can.c`

## 五、自测题

1. TCP OTA 为什么整帧一次性写入？多次小写实测慢多少？
2. Nagle 不禁用会怎样？（发送队列堆积 → netconn_write 永久阻塞 → OTA 挂死）
3. 流式逐帧处理解决什么问题？（多帧同段到达的帧错位）
4. 失步/超长帧怎么恢复？（整帧重同步）
5. HTTP 拉取为什么先 Ota_Reset？不 Reset 会怎样？（续传误命中）
6. HTTP 头部与 body 同段到达时怎么处理？（头尾进位）
7. 收包超时为什么重试 6 次而不是立即失败？
8. 两种 OTA 模型的适用场景差异？（服务器推 vs 客户端拉）
