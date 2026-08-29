# D00 源码研学 · 笔记 15 —— 外部内存池 / 网络配置 / CRC（SystemServices 收官）

> 精读对象：`SystemServices/ext_mem.c`(381) · `net_config.c`(60) · `crc16.c`(23)
> **SystemServices 25,290 行至此全部精读完毕**（除 wav_data.c 19k 行纯数据表、test_ext_mem.c 自检）

---

## 一、⭐ 外部 SRAM 统一内存池（ext_mem.c）—— LVGL 内存后端

**定位**：376KB 外部 SRAM（0x680A2000，见 mem_map.h）上的专用分配器——LVGL 对象池/图像缓存/大缓冲共用。

### 1.1 设计要点（头注释 5 条：顶级可靠性/可观测性）
1. **边界标记（Knuth）**：每块头尾各存 `size|flags`，释放时通过前/后邻居尾部标记 **O(1) 双向合并**，长期运行零碎片累积
2. **canary 越界检测**：头尾魔数 `0xC0FFEE01`，释放/巡检时校验——越界写第一时间暴露并计数（canary_fail），不静默崩溃
3. **8 字节对齐**：满足 LVGL 与 FPU/总线对齐访问要求
4. **线程安全**：FreeRTOS 互斥量（`EXT_MEM_NO_OS` 编译宏去除，供主机单测）
5. **可观测**：总量/已用/峰值/最大空闲块/分配失败/越界计数——GUI 面板与启动日志直读

### 1.2 块布局（8 对齐，最小块 24B）
```
+0        size_and_flags（bit0=FREE）
+4        magic 0xC0FFEE01
+8        载荷（空闲块前 4B 存 free-list next 池内偏移——32 位安全，不存绝对指针）
+size-8   size_and_flags 副本（尾部边界标记）
+size-4   magic（尾部 canary）
```

### 1.3 核心算法
| 操作 | 实现 |
| --- | --- |
| Alloc | **first-fit**：遍历 free-list 找首个 ≥need 块；剩余 ≥24B 则**分裂**（保持池连续铺满） |
| Free | 头部魔数校验 → **双重释放防护**（blk_is_free 拒绝）→ 尾部 canary 校验 → **后向合并**（下一块空闲则吸收）→ **前向合并**（O(1) 边界标记定位前块）→ 入 free-list |
| Realloc | 缩容**原地返回**（不缩块，避免碎片抖动）；扩容 alloc+memcpy+free |
| 防御 | `blk_valid`（池内+对齐+魔数）、GetStats 遍历遇链损坏停止不崩溃 |

**free-list next 存"池内偏移"而非指针**（:125-139）：32 位安全、抗指针算术错误。

## 二、网络配置持久化（net_config.c，60 行）

薄封装：`UsrStore_Get/Set/Erase(USR_KEY_NET_CFG)`——静态 IP/网关存 EEPROM 用户存储（复用 usr_store 日志式键值库）。启动 `NetConfig_Init` 恢复，`net ip/gw` 命令调用 Save（cmd_catalog.c:129-149）。

## 三、CRC-16/MODBUS（crc16.c，23 行）

位算法：初值 0xFFFF，右移 + 反转多项式 0xA001（`HOSTLINK_CRC_POLY`）——**HOSTLINK 帧校验专用**（protocol.c 调用）。

> ⚙️ 全仓 CRC 谱系（第 4 处）：crc32.c 查表+final-xor（YMODEM）｜bkup_crc32 / boot_param_crc 位算法无 final-xor（BOOT 持久化）｜crc16.c 位算法（HOSTLINK）——**各协议域自洽，互不混用**。

## 四、⭐ SystemServices 阶段总结（15 篇笔记全部覆盖）

| 域 | 模块 | 笔记 |
| --- | --- | --- |
| 事件框架 | event_bus（CCM 静态池）/ module（33 模块注册表） | 04 |
| HOSTLINK | data_link（双任务双队列 DMA）/ protocol（帧格式）/ crc16 | 05 |
| 变量 | var_manager（指针直挂+CCM）/ var_list（分片两遍式） | 06 |
| 日志/健康 | logger（流+DMA）/ watchdog（任务级）/ err_mgr（黑匣子）/ sysmon（SysTick 喂狗） | 06, 10 |
| 命令 | shell（行编辑）/ cmd_shell（统一框架）/ cmd_catalog（52 命令，Application） | 07 |
| 存储 | ext_store（坏区+双份）/ usr_store（EEPROM 日志）/ ext_mem（边界标记池）/ net_config | 09, 10, 15 |
| OTA | ota_agent（续传+确认，Application） | 08 |
| 外设服务 | la_*（逻辑分析仪）/ signal_gen / audio_svc / imu_svc / cam_link / touch_svc | 11-14 |

**SystemServices 三大纪律**（贯穿全部模块）：
1. **中断/ISR 纪律**：ISR 内无锁、无 malloc、无浮点；ISR 只做最短工作（收字节/发通知）
2. **长操作喂狗 + 超时**：任何可能卡死的循环都有超时或看门狗兜底（DMA 自愈/自检超时/BSY 守卫）
3. **持久化 = CRC 覆盖纪律**：crc32 字段绝不算自己；持久化算法一经发布不可替换

## 五、待读清单（下一课——转 Application 网络服务批）

- [x] SystemServices 全部（15 篇笔记）
- [ ] `Application/eth_app.c`(706)：以太网应用（LwIP 移植 + 状态机）
- [ ] `tcp_svc.c` / `icmp_svc.c` / `dns_svc.c` / `sntp_svc.c` / `mqtt_svc.c` / `http_svc.c`：网络服务
- [ ] `ota_tcp_svc.c` / `ota_http_svc.c` / `ota_can_svc.c`：OTA 传输
- [ ] `gui_app.c` + `gui_pages.c`(1876) + `gui_theme.c`：LVGL GUI
- [ ] `cmd_catalog.c` 剩余命令 + `data_agent.c` + `buzzer_app.c` + `key_app.c` + `led_app.c`

## 六、自测题

1. 边界标记为什么能 O(1) 合并？没有尾部标记需要怎样？（遍历）
2. canary 魔数 0xC0FFEE01 在哪些时机校验？越界写什么时候暴露？
3. free-list next 为什么存池内偏移而不是指针？
4. Realloc 缩容为什么原地返回不缩块？（碎片抖动）
5. ExtMem_GetStats 遍历时为什么先 blk_valid？（链损坏防御）
6. crc16 与 crc32.c 的差异？HOSTLINK 为什么用 CRC16 而 YMODEM 用 CRC32？
7. net_config 依赖 usr_store 的好处？（复用日志式键值库）
8. 全仓 4 个 CRC 实现各自的用途？（防混用）
