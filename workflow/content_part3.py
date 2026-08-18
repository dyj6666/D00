# -*- coding: utf-8 -*-
"""content_part3.py —— OTA 说明书内容模块 3：上位机 / 实测 / 排障 / FAQ / 附录"""

SECTIONS = [
# =====================================================================
("h1", "第 9 章  上位机工具链（动手篇）"),
("h2", "9.1  工具一览"),
("table",
 ["工具", "形态", "用途"],
 [
  ["OTA_Tool", "GUI（Python）", "版本库管理、串口升级、批量升级、阶段可视化"],
  ["ota_tcp_cli.py", "命令行", "以太网 TCP 推送（脚本化/CI）"],
  ["ota_tcp_push.ps1", "PowerShell 封装", "自动读 version.json，一行推送"],
  ["gen_keys.py", "命令行", "生成 ECDSA 密钥对（私钥注入环境变量）"],
  ["enable_rdp.py", "命令行", "启用 RDP 读保护脚本（发布前）"],
 ],
 [4.0, 4.0, 8.5]),
("h2", "9.2  快速上手：5 分钟完成一次 TCP 升级"),
("code",
"① 改版本：编辑 config/version.json\n"
"     { \"ota_version\": 213, \"ota_build\": 9181 }   ← build 必须比设备 PARAM last_build_no 大\n"
"② 构建固件：Keil UV4 -b（或 CI）\n"
"③ 推送：\n"
"     powershell -File workflow\\ota_tcp_push.ps1 -Ip 192.168.10.10\n"
"④ 观察：下载进度 → END 触发 BOOT → 约 30s 完成 → 设备重启进新固件\n"
"⑤ 验证：主界面固件卡显示 FW v213 b9181（PARAM last_build_no 同步更新）"),
("note", "最容易踩的坑：build 号没递增 → BOOT 防重放拒绝（Replay denied）→ 看起来“OTA 成功”但固件没变。"
         "每次推送前务必递增 ota_build。"),
("h2", "9.3  OTA_Tool GUI 操作"),
("li", "版本库刷新 → 选择固件版本（自动填文件/版本号/build）。", "① 版本库："),
("li", "串口 COM / 921600；私钥环境变量 OTA_PRIVKEY 注入。", "② 连接："),
("li", "打开串口 → 开始升级 → 阶段流程条 + BOOT 状态帧彩色日志。", "③ 升级："),
("li", "端口逗号分隔（COM5,COM13），串行预分配 build_no 后并发升级。", "④ 批量："),
("h2", "9.4  打包与签名（encrypt_and_sign 流程）"),
("code",
"encrypt_and_sign(APP.bin, version, build, chip_id):\n"
"  hdr = ota_header_t(magic=0x4F5441FE, version, size, iv, chip_id, build_no)\n"
"  key = derive_aes_key_from_uid(uid)          # 与 BOOT 一致\n"
"  cipher = AES_CTR_encrypt(key, iv, payload)  # 载荷加密\n"
"  digest = SHA256(hdr + cipher)\n"
"  sig = ECDSA_sign(privkey, digest)           # 私钥=环境变量 OTA_PRIVKEY\n"
"  return hdr + cipher + sig"),
("h2", "9.5  版本管理约定"),
("li", "version 语义：功能版本，只升不降（防回滚）。", "版本："),
("li", "build 语义：每次出包必须递增的流水号（防重放）。", "构建号："),
("li", "config/version.json 为单一事实源（CI/推送脚本自动读取）。", "单一事实源："),

# =====================================================================
("h1", "第 10 章  实测验证记录（全部真机）"),
("h2", "10.1  功能实测矩阵"),
("table",
 ["项目", "结果"],
 [
  ["HOSTLINK 运行时升级", "v13→v69 连续多轮成功（业务不中断）"],
  ["ETH-TCP 升级", "v213 build 9180→9187 连续 8 轮成功"],
  ["YMODEM 传统升级", "成功（含新包头兼容）"],
  ["回滚（PENDING 超限）", "[RB] erase=1 copy=1 成功"],
  ["BACKUP 自愈（RUN 损坏）", "自动恢复成功"],
  ["断点续传", "32160/64192 续传成功"],
  ["断电注入 4 阶段", "全部自动恢复"],
  ["密钥轮换", "新/旧公钥包均验证通过"],
 ],
 [7.0, 9.5]),
("h2", "10.2  安全校验实测"),
("table",
 ["场景", "BOOT 输出", "结论"],
 [
  ["同构建号重放", "[SEC] Replay denied! build=9180 last=9180", "防重放生效（真实案例）"],
  ["跨芯片", "[SEC] Chip mismatch", "芯片绑定生效"],
  ["版本降级", "[SEC] Rollback denied", "防回滚生效"],
  ["签名损坏", "[SEC] ECDSA verify failed", "验签生效"],
 ],
 [5.0, 8.5, 3.0]),
("h2", "10.3  编译与回归"),
("li", "BOOT/APP Keil AC5 构建 0 Error / 0 Warning。", "构建："),
("li", "CI（GCC 构建 + ctest 主机测试 + HOST 测试 + 后门扫描）全绿。", "CI："),

# =====================================================================
("h1", "第 11 章  已知边界与局限"),
("table",
 ["项", "现状", "影响/计划"],
 [
  ["RDP 读保护", "脚本就绪，发布前实机验证", "启用后 DAP 无法读 Flash（防提取）"],
  ["启动早期擦 PARAM", "寄存器级机制为推测，已架构规避", "避免启动早期擦参数扇区"],
  ["批量 UID 一致", "AES 密钥由 UID 派生", "同批次设备 UID 必须一致"],
  ["回滚深度", "BACKUP 仅上一版", "无 N-2 级回滚"],
  ["CAN 通道", "注册表就绪，服务未实现", "未来接入"],
  ["HTTPS", "HTTP 拉取（明文传输）", "包已加密签名，传输层明文可接受"],
 ],
 [4.0, 7.0, 5.5]),

# =====================================================================
("h1", "第 12 章  故障排查指南（症状 → 原因 → 解决）"),
("h2", "12.1  错误码速查表"),
("table",
 ["错误码/输出", "含义", "处理"],
 [
  ["Replay denied", "build_no 未递增（<= PARAM last_build_no）", "递增 ota_build 后重推"],
  ["Chip mismatch", "包 chip_id 与设备不符", "用目标设备 UID 重新打包"],
  ["ECDSA verify failed", "签名错误/私钥不符/包被篡改", "检查 OTA_PRIVKEY 与打包流程"],
  ["Rollback denied", "version 低于当前固件", "版本号只升不降"],
  ["0x1002 BACKUP failed", "备份当前 RUN 到 img_lib 失败", "检查外部 Flash；重推"],
  ["0x1003 APP erase failed", "擦除 RUN 失败", "复位后由 BACKUP 自愈；重推"],
  ["0x1004 APP write failed", "解密写入失败", "复位后由 BACKUP 自愈；重推"],
  ["TCP PUSH FAILED", "设备 :9020 不可达", "检查 ETH 链路/设备是否运行（复位后重试）"],
 ],
 [4.5, 6.5, 5.5]),
("h2", "12.2  常见故障排查步骤"),
("h3", "症状 1：推送显示成功但固件没变"),
("li", "build 号未递增 → 被防重放拒绝。", "第一检查："),
("li", "读设备 PARAM last_build_no（0x080E0014）对比推送 build。", "确认："),
("li", "看 BOOT 阶段是否有三短音（校验失败提示）。", "辅助："),
("h3", "症状 2：设备不响应 :9020"),
("li", "ping 192.168.10.10——ICMP 通但 :9020 不通 → 设备半死（TIM7 停）。", "判断："),
("li", "手动复位设备 → 系统完整运行 → 重推。", "解决："),
("h3", "症状 3：升级后启动黑屏/异常"),
("li", "启动确认未完成（PENDING 超限）→ 自动回滚。", "机制："),
("li", "检查 PARAM boot_state（1=NORMAL，2=PENDING）；观察是否回滚。", "确认："),
("h3", "症状 4：断点续传失败"),
("li", "会话槽被 BOOT 提交清理（正常）；或版本不一致（不同包不续传）。", "原因："),
("li", "重新 BEGIN 完整下载。", "解决："),
("h2", "12.3  诊断三板斧"),
("li", "DAP 读 PARAM（0x080E0000 起）：boot_state/count/error/build_no 一目了然。", "① 读参数区："),
("li", "COM5 串口日志：OTA Agent 打印 begin/data/end/entering BOOT；[SEC] 拒绝原因。", "② 看日志："),
("li", "DAP 读 Flash 向量表比对 bin（0x08010000 前 16B）确认固件是否真被替换。", "③ 验 Flash："),

# =====================================================================
("h1", "第 13 章  FAQ"),
("table",
 ["问题", "回答"],
 [
  ["升级时业务会中断吗？", "下载阶段不中断（APP 持续运行）；仅复位切换瞬间中断（<1s 级）。"],
  ["升级断电会变砖吗？", "不会。4 阶段断电注入全部实测恢复（续传/重应用/回滚/自愈）。"],
  ["固件包能跨设备刷吗？", "不能。AES 密钥由芯片 UID 派生，包与设备绑定。"],
  ["旧固件能重刷吗？", "不能。build 防重放 + version 防回滚双防线。"],
  ["APP 崩溃了还能升级吗？", "能。YMODEM 救援路径由 BOOT 直接接收（不依赖 APP）。"],
  ["升级后新固件有问题怎么办？", "PENDING 窗口内自动回滚（count 到 3）；BACKUP 自愈。"],
  ["如何确认当前固件版本？", "主界面固件卡显示 FW v213 b9181；或读 PARAM last_build_no。"],
  ["多次失败会锁死吗？", "回滚超限进入 RECOVERY 恢复模式，强制重刷即可解锁。"],
 ],
 [5.5, 11.0]),

# =====================================================================
("h1", "第 14 章  总结与展望"),
("p", "D00 OTA 系统以“三端闭环 + 双区布局 + 外部回滚源 + 端到端安全 + 多通道传输 + 全链路自愈”"
     "构建了工业级固件升级的完整范式。其设计可迁移到任何带内部/外部 Flash 的 MCU 平台。"),
("li", "CAN 通道落地、RDP 发布前实机验证、N-2 级回滚、批量集群管理。", "展望："),
("li", "把“断电注入 4 阶段”固化为 CI 回归项，每次发版自动跑。", "工程化："),

# =====================================================================
("pagebreak",),
("h1", "附录 A  关键数据结构（源码级）"),
("h2", "A.1  ota_header_t（32B）"),
("code",
"typedef struct {\n"
"    uint32_t magic;      /* 0x4F5441FE：OTA 包标识 */\n"
"    uint32_t version;    /* 固件版本：防回滚（只升不降） */\n"
"    uint32_t size;       /* 密文长度：容量校验 */\n"
"    uint8_t  aes_iv[12]; /* AES-CTR 初始向量（+4B 零=16B） */\n"
"    uint32_t chip_id;    /* 目标芯片 ID：防跨芯片 */\n"
"    uint32_t build_no;   /* 构建号：防重放（严格递增） */\n"
"} ota_header_t;"),
("h2", "A.2  boot_param_t（PARAM 双份）"),
("code",
"typedef struct {\n"
"    uint32_t magic;            /* 参数区魔数 */\n"
"    uint32_t boot_state;       /* 1=NORMAL 2=PENDING 3=RECOVERY 4=UPGRADE */\n"
"    uint32_t boot_count;       /* 启动计数（PENDING 窗口计数） */\n"
"    uint32_t rollback_count;   /* 回滚计数（超限进 RECOVERY） */\n"
"    uint32_t last_error;       /* 上次升级错误码（0x1001-0x1004/SEC err） */\n"
"    uint32_t last_build_no;    /* 已接受最大构建号（防重放基准） */\n"
"    uint32_t crc32;            /* 覆盖 magic..last_build_no */\n"
"} boot_param_t;   /* slot0@0x080E0000  slot1@0x080E0400 */"),
("h2", "A.3  OTA 会话槽（断点续传）"),
("code",
"struct { magic 0x4F54414D; version; total; received; crc32; }\n"
"（PARAM 区 0x080E2000 起；BOOT 升级提交后失效）"),
("h2", "A.4  TCP 帧"),
("code",
"帧 = magic(0x5A) | cmd | len(2B LE) | payload | crc8\n"
"cmd: 0x01 BEGIN / 0x02 DATA / 0x03 END / 0x04 STATUS / 0x05 RESET\n"
"DATA 载荷 = offset(4B LE) + 240B 块（帧总长上限 249B）"),

# =====================================================================
("h1", "附录 B  术语表"),
("table",
 ["术语", "说明"],
 [
  ["OTA", "Over-The-Air，空中/远程固件升级"],
  ["BOOT", "启动引导器（0x08000000，64KB）——校验/升级/回滚执行者"],
  ["RUN", "APP 运行区（0x08010000，832KB）"],
  ["PARAM", "参数区（0x080E0000，128KB）——状态/防重放/续传"],
  ["ota_dl", "外部 Flash 下载暂存槽（2MB，单槽 1MB）"],
  ["img_lib", "外部 Flash 回滚源槽（头 4KB + 数据 832KB）"],
  ["PENDING", "升级待确认状态（新固件首启确认后转 NORMAL）"],
  ["NORMAL", "正常运行态"],
  ["RECOVERY", "恢复模式（回滚超限，等待强制重刷）"],
  ["UPGRADE", "升级模式（执行升级流程）"],
  ["HOSTLINK", "上位机串口链路协议（UART1 921600）"],
  ["YMODEM", "传统串口文件传输协议（BOOT 救援路径，115200）"],
  ["AES-CTR", "Advanced Encryption Standard，计数器模式（流式加解密）"],
  ["ECDSA", "椭圆曲线数字签名算法（P-256）"],
  ["SHA-256", "安全哈希算法（32B 指纹）"],
  ["UID", "芯片唯一 ID（AES 密钥派生输入）"],
  ["RDP", "Read-out Protection（读保护，防固件提取）"],
  ["IWDG", "独立看门狗（硬件，LSI 时钟）"],
  ["WDOG", "任务级看门狗（软件，SysMon）"],
  ["BKP", "备份寄存器（RTC 域，升级标志 0x5A5A 等）"],
 ],
 [3.5, 13.0]),

("h1", "附录 C  参考文档与源码索引"),
("table",
 ["资料", "位置"],
 [
  ["OTA 架构全貌", "APP/APP/Doc/OTA_ARCHITECTURE.md"],
  ["工程复盘日志（12.x OTA 章节）", "APP/APP/Doc/ENGINEERING_LOG.md"],
  ["上位机用户指南", "docs/OTA_TOOL_USER_GUIDE.md"],
  ["BOOT 升级主流程", "BOOT/BOOT/BootApp/boot_app.c"],
  ["安全校验", "BOOT/BOOT/BootServices/security.c"],
  ["参数区管理", "BOOT/BOOT/BootServices/boot_param.c/h"],
  ["回滚备份", "BOOT/BOOT/BootServices/ota_backup.c"],
  ["外部 Flash 驱动", "BOOT/BOOT/BootServices/esp_flash.c"],
  ["APP 下载核心", "APP/APP/Application/ota_agent.c"],
  ["TCP 通道", "APP/APP/Application/ota_tcp_svc.c"],
  ["传输注册表", "APP/APP/Application/ota_transport.h / ota_mgr.c"],
  ["命令行推送", "APP/APP/Script/ota_tcp_cli.py"],
 ],
 [6.0, 10.5]),
]
