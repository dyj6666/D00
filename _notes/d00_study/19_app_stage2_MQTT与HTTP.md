# D00 源码研学 · 笔记 19 —— MQTT 遥测与 HTTP 状态服务

> 精读对象：`Application/mqtt_svc.c`(298) · `http_svc.c`(193)
> 阶段 2 应用层第 4 批：物联网双通道（MQTT 推送 + HTTP 查询）

---

## 一、⭐ MQTT 工业遥测（mqtt_svc.c）—— lwIP MQTT 客户端

### 1.1 架构
```
broker 地址 EEPROM 持久化（6B：IP4+port）→ MqttSvc_Connect
  → tcpip_callback(mqtt_do_connect)（tcpip 线程内连接）
  → 连接回调 mqtt_conn_cb（状态机：ACCEPTED/DISCONNECTED/ERR）
  → 遥测定时器（5s）→ 组 JSON → tcpip_callback(mqtt_do_pub)
  → 订阅回调 mqtt_inpub_cb / mqtt_indata_cb（打印主题/payload）
```

### 1.2 ⭐ 血泪注释（:114-117）—— lwIP MQTT 的陷阱
```
"注意：mqtt_client_connect 内部 memset 整个 client 结构，会清空此前设置的
 inpub 回调——必须在 connect 之后（tcpip 线程内）重设，否则收到 PUBLISH 时
 data_cb 为空指针 → INVSTATE 崩溃。"
```
→ **连接后必须重设 inpub 回调**（mqtt_set_inpub_callback 在 connect 之后调用）——这是 lwIP MQTT API 的隐藏契约。

### 1.3 遥测 JSON（:171-180）
```json
{"v":版本, "up":运行秒, "heap":堆余量, "rx":ETH收包, "tx":ETH发包,
 "icmp_rx":ICMP收, "icmp_tx":ICMP发}   → 主题 d00/status，QoS0
```
- **静态缓冲**（s_topic 48B + s_payload 96B）+ 静态 op 块——无堆分配
- **tcpip_callback 投递**：定时器回调（Tmr Svc 上下文）只组包不发网络

### 1.4 客户端配置
- client_id "D00-F407"、keep_alive 30s、无认证/will
- broker 保存 `USR_KEY_MQTT_BROKER`（IP4+port 6B）
- 状态机：0=断开 1=连接中 2=已连接 3=错误（+计数）

## 二、HTTP 状态服务（http_svc.c）—— :8080 单连接串行

### 2.1 架构
```
http_task（1024B 栈，BelowNormal）
  → netconn listen :8080 → accept（500ms 超时不阻塞）→ http_handle（串行，单连接）
      → 读请求行（netconn_recv + 1.5s 超时）
      → GET /api/status → JSON；GET / → HTML；其他 → 404
      → 组响应（HTTP/1.0 + Content-Length + Connection: close）
```

### 2.2 关键设计
1. **静态缓冲**（:106-108）：`static char buf[1024]` + `static char body[1024]`——单连接串行处理，**避免大局部数组压爆任务栈**（静态缓冲在 BSS，任务栈只留指针）
2. **串行单连接**（头注释）：避免并发竞争——同一时刻只服务一个客户端
3. **超时兜底**（:127-131）：连接无数据/超时回 400 并关闭——**客户端不悬挂**
4. **accept 不阻塞**（:168）：500ms 超时 + 5ms 延迟——单请求不拖垮服务（可响应 enable/disable）
5. **状态聚合**（http_build_body :46-102）：ver/uptime/heap/link/IP/rx/tx/icmp/usr 存储统计/mqtt 状态/RTC 时间——JSON + HTML 双格式
6. **enabled 开关**：`http on|off` 命令可禁用

## 三、设计亮点

1. **tcpip_callback 投递纪律**：定时器/任务上下文只组包，网络操作全在 tcpip 线程
2. **lwIP MQTT 契约血泪**：connect 后重设回调（INVSTATE 崩溃教训）
3. **静态缓冲模式**：HTTP 大缓冲放 BSS 不放栈
4. **超时兜底**：任何网络读都有超时，绝不悬挂
5. **统一状态聚合**：HTTP/MQTT/sysmon 共用 EthApp/ICMP 统计

## 四、待读清单（下一课）

- [x] mqtt_svc / http_svc（本轮完成）
- [ ] `ota_tcp_svc.c`(261) / `ota_http_svc.c` / `ota_can_svc.c`(241)：OTA 传输三件套
- [ ] `gui_app.c`(351) / `gui_pages.c`(1876) / `gui_theme.c`：LVGL GUI
- [ ] `data_agent.c` / `buzzer_app.c` / `key_app.c` / `led_app.c` / `cmd_catalog.c` 剩余

## 五、自测题

1. lwIP MQTT 的 inpub 回调为什么必须在 connect 之后设置？不设会怎样？
2. 遥测定时器为什么用 tcpip_callback 投递？直接在定时器回调里 mqtt_publish 会怎样？
3. HTTP 为什么用静态缓冲而不是局部数组？（栈安全）
4. HTTP 单连接串行解决了什么问题？并发会怎样？
5. accept 超时 500ms + vTaskDelay(5ms) 的意义？（服务可响应开关命令）
6. 连接无数据时为什么回 400 而不是挂着？（客户端悬挂）
7. MQTT JSON 里哪些字段来自哪些模块？（聚合架构）
8. MQTT 状态机 0-3 各代表什么？disconnect_cnt/err_cnt 统计什么？
