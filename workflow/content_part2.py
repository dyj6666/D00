# -*- coding: utf-8 -*-
"""content_part2.py —— OTA 说明书内容模块 2：通道 / BOOT / 流程 / 可靠性"""

SECTIONS = [
# =====================================================================
("h1", "第 5 章  多通道传输架构（五条路，一个核心）"),
("h2", "5.1  传输抽象层：为什么这样设计"),
("p", "OTA 的下载核心（Ota_Begin/Data/End——擦暂存区、写块、断点续传、触发升级）与具体传输通道"
     "完全解耦。通道只做两件事：收字节、把字节按帧喂给核心。新增一条通道（如 CAN）只需实现服务"
     "并调用 OtaMgr_Register 注册一行。"),
("code",
"typedef enum {\n"
"    OTA_TRANSPORT_UART     = 1,  /* HOSTLINK 串口（data_link 通道） */\n"
"    OTA_TRANSPORT_ETH_TCP  = 2,  /* 以太网 TCP 服务器（:9020） */\n"
"    OTA_TRANSPORT_ETH_HTTP = 3,  /* 以太网 HTTP 客户端（拉取） */\n"
"    OTA_TRANSPORT_CAN      = 4,  /* CAN 总线 OTA（ota_can_svc 运行时注册） */\n"
"} ota_transport_id_t;\n"
"\n"
"/* OtaMgr_Init 登记：UART / ETH-TCP / ETH-HTTP；\n"
" * CAN 由 OtaCanSvc_Init 运行时注册（available=1）；\n"
" * 注册表容量 OTA_TRANSPORT_MAX=6；ota status 命令逐条展示 */"),
("h2", "5.2  通道一：HOSTLINK（UART1，921600）"),
("li", "上位机数据链路协议（data_link）承载；帧 = AA 55 + cmd + len + payload + CRC16-LE；"
        "OTA 命令号：0x08 BEGIN / 0x09 DATA / 0x0A END / 0x0B STATUS / 0x0D RESET（0x0C 为 BOOT 状态广播帧）。", "载体："),
("li", "载荷字节序为小端（LE）：BEGIN(version+size 各 4B LE) → DATA(offset 4B LE + ≤240B 块) → STATUS 查询 → END。", "会话："),
("li", "默认推荐；开发调试与有线现场升级。", "适用："),
("h2", "5.3  通道二：ETH-TCP（:9020）——字节级协议"),
("p", "APP 内 ota_tcp_svc 作为 TCP 服务器监听 9020；ota_tcp_cli 作为客户端推送。帧格式："),
("code",
"通用帧 = magic(0x5A,1B) | cmd(1B) | len(2B 大端) | payload | crc8(1B)\n"
"cmd 定义：\n"
"  0x01 BEGIN    payload: version(4B) + size(4B)（均大端；无 build 字段）\n"
"  0x02 DATA     payload: offset(4B 大端) + 240B 固件块\n"
"  0x03 END      payload: 空（触发升级）\n"
"  0x04 STATUS   payload: 空（服务端回 status+已收/总长）\n"
"  0x05 RESET    payload: 空（触发复位）\n"
"CRC8 = poly 0x07 初值 0，覆盖 cmd+len+payload；ACK 帧 cmd=0x80\n"
"DATA 帧长度上限 OTA_TCP_MAX_FRAME = 4头+4偏移+240数据+1CRC = 249B"),
("table",
 ["阶段", "发起方", "帧", "接收方动作"],
 [
  ["握手", "客户端", "BEGIN(version,size)", "擦外部 ota_dl / 恢复同版本续传会话；回 ACK"],
  ["传输", "客户端", "DATA×N（每块 240B）", "Ota_Data 写槽；每帧回 ACK(received)"],
  ["查询", "任一方", "STATUS", "返回已收字节（断点确认）"],
  ["完成", "客户端", "END", "校验完整性 → 写 PARAM UPGRADE + BKP 标志 → 复位"],
  ["遥控", "客户端", "RESET", "立即复位（救援场景）"],
 ],
 [3.0, 2.5, 6.0, 5.0]),
("note", "实战：ota_tcp_push.ps1 封装（自动读 version.json 的 version/build，调用 ota_tcp_cli 推送）。"
         "注意：build 必须比 PARAM last_build_no 大，否则被防重放拒绝——这是 OTA 失败的第一排查点。"),
("h2", "5.4  通道三：ETH-HTTP（客户端拉取）"),
("li", "APP 作为 HTTP 客户端向服务器拉取固件包（ota_http_svc）。", "模式："),
("li", "适合服务器统一分发（固定 URL + 版本查询）。", "适用："),
("h2", "5.5  通道四：CAN（已实现，1Mbps）"),
("li", "ota_can_svc 完整实现（CAN 帧 ↔ Ota_Begin/Data/End 协议翻译），运行时注册（available=1）。", "现状："),
("li", "控制帧 0x200（主机→设备）：BEGIN/END/STATUS/ABORT；数据帧 0x201（240B 块，行帧规约）；"
        "应答 0x210、块 ACK 0x211（设备→主机，逐块背压，携带已收字节数）。", "帧 ID："),
("li", "BEGIN 载荷：size(LE32) + version(LE16)；5s 无数据超时自动 ABORT；乱序/超长整组丢弃。", "规约："),
("li", "车载/工业总线无网络环境的首选；BSP CAN1 1Mbps 就绪即用。", "适用："),
("h2", "5.6  通道五：YMODEM（BOOT 直接接收——救援路径）"),
("li", "触发：BKP 标志 / PARAM UPGRADE → BOOT 进升级模式；若 ota_dl 无有效预下载包 → 进入 YMODEM 等待。", "触发："),
("li", "BOOT 经 UART1（115200）接收 YMODEM 包，按 4KB 扇区边擦边写外部 ota_dl 槽 → 同一套校验/备份/解密/写流程。", "流程："),
("li", "APP 无法启动时的最后手段（配合 BOOT0 + UART 恢复）。", "定位："),
("h2", "5.7  通道选型决策表"),
("table",
 ["场景", "推荐通道", "原因"],
 [
  ["开发调试", "HOSTLINK", "921600 高速、双向可视"],
  ["现场有线", "HOSTLINK / ETH-TCP", "稳定可控"],
  ["远程在线", "ETH-TCP / ETH-HTTP", "网络可达即可"],
  ["总线环境", "CAN（1Mbps）", "无网络依赖"],
  ["APP 崩溃", "YMODEM（BOOT 直收）", "不依赖 APP"],
 ],
 [4.0, 5.5, 7.0]),

# =====================================================================
("h1", "第 6 章  BOOT 启动与升级状态机（逐状态讲透）"),
("h2", "6.1  状态定义"),
("table",
 ["状态", "值", "含义"],
 [
  ["NORMAL", "1", "正常运行态：BOOT 校验魔数 → 跳 APP"],
  ["PENDING", "2", "升级待确认：新固件已写，等待 APP 启动确认（防回滚窗口）"],
  ["RECOVERY", "3", "恢复模式：回滚超限；无有效包时自动归一 NORMAL 回旧固件（有意妥协：宁用旧固件不砖机，升级通道随时可恢复）"],
  ["UPGRADE", "4", "升级模式：执行升级流程"],
 ],
 [3.5, 2.0, 11.0]),
("h2", "6.2  启动状态机（复位后的完整决策树）"),
("code",
"复位\n"
"  │\n"
"  ├─ BKP标志=UPGRADE 或 param.state=UPGRADE(4) ──▶ 升级模式\n"
"  ├─ param.state=PENDING(2) 且 count>=3 ────────▶ 回滚（BACKUP→RUN）\n"
"  ├─ param.state=PENDING 且 count<3 ───────────▶ 待确认（count++ → 跳 APP）\n"
"  ├─ param.state=RECOVERY(3) ──────────────────▶ 恢复模式（等固件；无有效包\n"
"  │                                             自动归一 NORMAL 回旧固件）\n"
"  └─ param.state=NORMAL(1)\n"
"        ├─ RUN 魔数有效 ──▶ 跳 APP\n"
"        ├─ RUN 无效且 BACKUP 有效 ──▶ BACKUP→RUN 复制 → 跳 APP（自愈）\n"
"        └─ 均无效 ──▶ 升级模式"),
("note", "自愈路径：即使 RUN 被完全破坏，只要外部 img_lib 备份有效，BOOT 会自动恢复——"
         "这是“断电/坏写也不变砖”的根本保障。"),
("h2", "6.3  升级模式内部流程（boot_apply_download——逐步）"),
("table",
 ["步骤", "动作", "失败处理", "错误码"],
 [
  ["1 探测", "检查外部 ota_dl 包头（magic 0x4F5441FE 且总长 ≤1MB）→ 直通应用；无包回退 YMODEM", "无包不卡升级模式", "—"],
  ["2 校验", "芯片 ID → 防重放(build_no>last) → 容量(firmware_size≤832KB) → SHA256+ECDSA(双公钥) → 版本防回滚", "归一化参数，跳回 APP 支持重下/续传", "SEC err"],
  ["3 备份", "RUN 有效则全量备份到外部 img_lib（写后读回逐块校验）", "备份失败中止升级（RUN 未破坏）", "0x1002"],
  ["4 擦除", "擦除 RUN 区（扇区 4-10）", "RUN 已破坏，复位后 BACKUP 自愈", "0x1003"],
  ["5 写入", "AES-CTR 流式解密写入 RUN，随后校验向量表（SP/PC）", "RUN 损坏，复位后 BACKUP 修复 / 向量非法", "0x1004/0x1005"],
  ["6 提交", "写魔数/版本 → PARAM 置 PENDING+count=1 → 清会话槽+擦外部下载槽（备份保留） → 状态帧广播 → 重启", "PENDING 持久化（断电安全）", "—"],
 ],
 [2.0, 8.5, 4.5, 2.0]),
("h2", "6.4  状态帧广播（上位机可视化）"),
("li", "BOOT 应用阶段经 UART1 广播 0x0C 帧（阶段+错误码+版本，临时切 921600）。", "机制："),
("li", "上位机解析 → 阶段流程条显示真实推进：VERIFY → BACKUP → ERASE → WRITE → COMMIT → DONE。", "阶段："),
("h2", "6.5  看门狗与长操作"),
("li", "BOOT IWDG 128 分频（250Hz）+ 4095 ≈16.4s——覆盖 832KB 内部擦写 + 外部备份长操作。", "窗口："),
("li", "Flash 擦/写/复制长操作在 SRAM 内执行（.ramfunc）并逐块喂狗——防止长操作触发看门狗复位。", "喂狗："),

# =====================================================================
("h1", "第 7 章  运行时 OTA 全流程（一次升级的完整时序）"),
("h2", "7.1  阶段总览"),
("code",
"阶段① 下载（业务不中断）     APP 运行中，多通道收包写外部 ota_dl\n"
"阶段② 提交                   END → PARAM UPGRADE + BKP 标志 → 复位\n"
"阶段③ BOOT 应用              探测→校验→备份→擦除→解密写→PENDING→重启\n"
"阶段④ 启动确认               APP 首启 → ota_confirm_startup → NORMAL"),
("h2", "7.2  阶段①下载（Ota_Begin/Data/End 核心）"),
("table",
 ["接口", "入参", "行为"],
 [
  ["Ota_Begin", "version, size", "若已在接收则拒绝；擦外部 ota_dl（或恢复同版本会话）；置 OTA_ST_RECEIVING"],
  ["Ota_Data", "offset, chunk", "写外部槽对应偏移；每 16 块（3840B）持久化一次进度到会话槽（断点粒度 3840B）"],
  ["Ota_End", "—", "校验接收完整 → 写参数 UPGRADE + BKP 标志 → BSP_SystemReset"],
 ],
 [3.0, 4.0, 9.5]),
("note", "业务不中断的关键：下载期间 APP 一切照常（网络/界面/外设），仅 Flash 写入短暂占用。"),
("h2", "7.3  断点续传（跨复位）"),
("li", "会话槽记录 version/total/received；BEGIN 检测同版本会话 → 不擦除，从 received 偏移续传。", "机制："),
("li", "恢复路径重置 Flash 控制器状态（清错误标志 + Unlock/Lock），避免续传写入 BSY 卡死。", "恢复："),
("li", "实测：32160/64192 字节断点续传成功。", "实证："),
("h2", "7.4  启动确认（防回滚闭环）"),
("code",
"APP 启动 → ota_confirm_startup():\n"
"  读 PARAM boot_param\n"
"  若 boot_state == PENDING：\n"
"     boot_state = NORMAL   /* 新固件正常运行，确认成功 */\n"
"     boot_param_save()     /* 双份写 */\n"
"     擦除外部 img_lib 备份头（4KB）→ 回滚源失效，杜绝旧备份复活\n"
"  否则：无操作（正常运行态）\n"
"效果：确认前回滚源可用；确认后正式切换（A/B 闭环完成）"),
("h2", "7.5  一次典型 TCP 升级的完整时间线（实测 build 9180→9181）"),
("code",
"[00:00.0] ota_tcp_cli 连接 192.168.10.10:9020\n"
"[00:00.2] BEGIN v213 size=459204 build=9181\n"
"[00:00.5] DATA ×1914（1913×240B + 尾块 84B，带 offset+CRC8）\n"
"[00:04.1] STATUS 459204/459204 → state=1（完整）\n"
"[00:04.2] END → APP 写 UPGRADE → 复位\n"
"[00:05.0] BOOT：防重放校验 build=9181>last=9180 通过 → 备份/擦除/解密写\n"
"[00:35.0] PENDING → 重启 → 跳新 APP → 启动确认 → NORMAL\n"
"[00:36.0] 升级完成，业务恢复"),

# =====================================================================
("h1", "第 8 章  可靠性设计（把“变砖”彻底排除）"),
("h2", "8.1  断电保护矩阵（全生命周期）"),
("table",
 ["断电时刻", "现场状态", "上电恢复路径", "验证"],
 [
  ["下载中", "外部 ota_dl 部分写入", "会话槽续传（同版本）", "✅ 实测"],
  ["备份后", "img_lib 已备份，RUN 未动", "重启重新应用（RUN 仍有效）", "✅ 实测"],
  ["擦除后", "RUN 全 0xFF", "BOOT 探测 ota_dl → 重新应用", "✅ 实测"],
  ["写入后", "RUN 部分写入（无魔数）", "重新应用 / BACKUP 回滚", "✅ 实测"],
  ["提交后", "PENDING 持久化", "启动确认（count++ 直到确认或回滚）", "✅ 实测"],
 ],
 [3.0, 5.5, 6.5, 2.0]),
("note", "设计哲学：任何时刻的“系统状态”要么已持久化（PARAM/外部 Flash），要么可重建（重下/重备份）——"
         "不存在“中间态不可恢复”的窗口。"),
("h2", "8.2  看门狗体系（三层）"),
("table",
 ["层", "实现", "窗口", "防什么"],
 [
  ["BOOT IWDG", "128 分频（250Hz）+4095 + SRAM 内喂狗", "≈16.4s", "BOOT 长操作卡死"],
  ["APP IWDG", "32 分频 + SysTick 钩子喂狗", "≈4.1s", "APP 卡死/任务饿死"],
  ["任务级 WDOG", "SysMon 监控各任务心跳，超时统一错误管理", "5s（可配）", "单任务停滞"],
 ],
 [3.5, 7.0, 2.5, 3.5]),
("note", "调试/取证模式（APP_DEBUG_MODE=1）：关闭 IWDG/WDOG/ERR 软复位——死机现场保留供 DAP 取证。"),
("h2", "8.3  防呆与自愈清单"),
("li", "芯片绑定、版本/构建号校验、参数区 CRC、双份冗余。", "防呆："),
("li", "校验失败归一化参数跳回 APP；RUN 损坏由 BACKUP 自愈；均无效进升级模式。", "自愈："),
("li", "BOOT 升级提交后失效全部会话槽（防旧包续传）。", "清理："),
("h2", "8.4  已知风险与规避（诚实记录）"),
("li", "批量升级要求同批次设备 UID 一致（AES 密钥由 UID 派生）。", "UID 依赖："),
("li", "BACKUP 仅保留上一版（无 N-2 级回滚）。", "回滚深度："),
("li", "启动早期擦参数扇区 BSY 卡死为“证据充分的推测”，已通过架构规避（避免启动早期擦 PARAM）。", "BSY 卡死："),
]
