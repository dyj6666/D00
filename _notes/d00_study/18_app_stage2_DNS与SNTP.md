# D00 源码研学 · 笔记 18 —— DNS 与 SNTP 服务

> 精读对象：`Application/dns_svc.c`(130) · `sntp_svc.c`(225)
> 阶段 2 应用层第 3 批：域名解析 + 时间同步（网络基础服务）

---

## 一、DNS 服务（dns_svc.c）

### 1.1 架构
```
服务器地址持久化（EEPROM usr_store）→ lwIP dns_setserver 配置
DnsSvc_Resolve(host, timeout, out)：
  → dns_gethostbyname（异步）
      · 同步命中（ERR_OK）：直接返回
      · 异步进行（ERR_INPROGRESS）：信号量阻塞等待 dns_found_cb（回调里 Release）
      · 失败码细分：-1 参数 / -2 立即失败 / -3 超时 / -4 NXDOMAIN
```

### 1.2 关键设计
1. **回调-信号量桥接**（dns_found_cb :30-46）：lwIP DNS 回调（tcpip 线程）→ 信号量唤醒调用方任务——异步转同步
2. **单实例请求块**（s_req 静态）：串行调用安全（一次只发一个解析请求）
3. **服务器 EEPROM 持久化**：`dns server <ip>` 保存，上电恢复
4. **错误码细分**：超时/NXDOMAIN/参数区分——shell 可诊断

## 二、⭐ SNTP 时间同步（sntp_svc.c）

### 2.1 核心流程
```
UDP netconn → :1123（★ 本地 NTP 服务器 HOST/ntp_server.py）
  → 48B NTP 请求（LI=0 VN=3 MODE=3 client）
  → 解析应答 [40..43] 的 NTP 秒数
  → local = ntp - 2208988800（1900→1970）+ 28800（UTC+8）
  → epoch_to_ymd（Hinnant civil_from_days 算法）→ 写 RTC
```

### 2.2 血泪注释（:189-192）—— 最重要的一课
```
"注意：不要改成'失败后短周期重试'——实测 netconn 反复调用会让 lwIP 在
 第二次同步时挂死（看门狗复位→BOOT 回滚）。上电实时校准改由 SntpSvc_Sync
 单次调用 + ETH 就绪后再触发的方式保证。"
```
→ **定时任务绝不短周期重试网络操作**：lwIP netconn 反复调用会挂死（触发了整个回滚链）。这是"失败重试策略"的经典反面教材——重试必须退避或由外部事件触发。

### 2.3 细节
- **端口 1123 而非 123**（:22-24）：PC 与板子直连无外网，Windows 的 123 被 w32time 占用——本地 NTP 服务器用 1123
- **栈 1024 血泪**（:221）：峰值 ~752B（netconn 路径），768B 余量仅 16B 太险
- **自动同步**：上电 5s 后首同步，之后每小时一次（SNTP_AUTO_PERIOD_S=3600）
- **星期计算**（:53-58）：1970-01-01 为周四，`wd=(days+4)%7`，0=周日 → HAL 星期（1=周一..7=周日）
- **UDP 用 netconn_send 而非 write**（:93）：UDP 无连接语义注释

## 三、设计亮点

1. **异步转同步**：回调+信号量桥接——任务友好接口
2. **失败重试纪律**：不短周期重试网络操作（挂死教训）——重试策略的工程课
3. **本地 NTP 服务器配套**：HOST 侧 ntp_server.py（直连无外网场景）
4. **时间算法自实现**：civil_from_days 公历算法 + 星期推导——不依赖 C 库
5. **EEPROM 持久化**：DNS/SNTP 服务器地址统一走 usr_store

## 四、待读清单（下一课）

- [x] dns_svc / sntp_svc（本轮完成）
- [ ] `mqtt_svc.c`(278) / `http_svc.c` / `ota_tcp_svc.c` / `ota_http_svc.c` / `ota_can_svc.c`
- [ ] `gui_app.c` / `gui_pages.c`(1876) / `gui_theme.c`：LVGL GUI
- [ ] `data_agent.c` / `buzzer_app.c` / `key_app.c` / `led_app.c` / `cmd_catalog.c` 剩余

## 五、自测题

1. DNS 解析如何把 lwIP 异步回调转成同步阻塞？信号量谁放谁收？
2. SNTP 为什么用 1123 端口？Windows 123 端口发生了什么？
3. "失败后短周期重试"曾导致什么？（完整事故链：重试→lwIP 挂死→看门狗→BOOT 回滚）
4. epoch_to_ymd 的 civil_from_days 算法输入输出是什么？
5. UDP 发送为什么用 netconn_send 不用 netconn_write？
6. SNTP 任务栈 1024 的依据是什么？（峰值 752B）
7. 时间戳换算：NTP 秒 → 本地秒 → 时分秒/年月日/星期的完整链路？
8. DNS 错误码 -2/-3/-4 分别代表什么？
