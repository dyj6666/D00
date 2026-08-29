# D00 源码研学 · 笔记 01 —— BOOT 启动状态机与升级流水线

> 精读对象：`BOOT/BOOT/BootApp/boot_app.c`（682 行）+ `Config/boot_config.h`（78 行）
> 配套：`docs/D00_OTA_System_Overview.pdf` · AGENTS.md 硬件约定

---

## 一、BOOT 架构地图（自研 5,000 行 = BootApp 604 + BootServices 4,397）

```
BOOT/BOOT/
├── BootApp/  boot_app.c/h        ← 启动状态机 + 升级主流程（本次精读）
├── BootServices/
│   ├── security.c/h              ← 安全校验+解密总入口（verify_and_decrypt）
│   ├── aes.c / my_sha256.c / uECC.c   ← 加密三件套（AES-CTR / SHA256 / ECC 验签）
│   ├── ymodem.c + ymodem_port.c  ← YMODEM 协议 + UART 移植层
│   ├── ota_backup.c              ← 外部 Flash 备份/回滚（img_lib 槽）
│   ├── ota_source.c              ← 升级包源探测（外部 ota_dl 槽）
│   ├── esp_flash.c               ← 外部 NOR Flash 驱动（W25Q 类）
│   ├── flash_if.c                ← 内部 Flash 擦写封装
│   └── boot_param.c              ← 参数区读写（双份冗余 + CRC32）
├── Config/boot_config.h          ← 全部分区/状态/阈值定义（本次精读）
└── Core/                         ← CubeMX 框架（时钟/串口/看门狗）
```

## 二、Flash 分区方案 B（boot_config.h:6-16）

| 区 | 地址 | 大小 | 说明 |
| --- | --- | --- | --- |
| BOOT | 0x08000000 | 64KB（扇区0-3） | 引导程序本身 |
| RUN | 0x08010000 | **832KB**（扇区4-10） | APP 固件（原 BACKUP+DOWNLOAD 并入，上限 320KB→832KB） |
| PARAM | 0x080E0000 | 128KB（扇区11） | 启动标志/回滚计数/升级日志（参数槽 +0/+1024 双份） |
| 外部 ota_dl | ESP_OTA_BASE | 1MB | **下载暂存**（YMODEM/运行时 OTA 写入目标） |
| 外部 img_lib | ESP_BACKUP_BASE | — | **回滚源**（升级前备份当前 RUN） |

**关键点**：回滚源与下载槽全部外移到外部 Flash（方案 B），内部只留 3 区。
魔数约定：`APP_VALID_MAGIC = 0x4F54412E` @ `0x080DFFF8`（RUN 尾部 -8），版本号紧随其后。

## 三、启动状态机（boot_config.h:43-50）

```
NORMAL ──(APP 请求升级: BKP_DR1==0x5A5A 或 param)──▶ UPGRADE ──▶ 升级流水线
NORMAL ──(新固件写入)──▶ PENDING(boot_count=1..3) ──(超3次)──▶ 回滚
PENDING ──(APP 启动确认)──▶ NORMAL（由 APP 侧清零）
回滚超限(5次) ──▶ RECOVERY（等待上位机强制重刷）
```

- `MAX_BOOT_TRIES = 3`：新固件最多启动 3 次，未确认则回滚
- `MAX_ROLLBACK_COUNT = 5`：连续回滚 5 次进 RECOVERY（防"坏固件反复刷"死循环）
- 参数区双份冗余（`PARAM_SLOT_OFFSET 1024`）+ CRC32，防单点损坏

## 四、BootApp_Run 决策树（boot_app.c:590-682）

```
BootApp_Run
├─ 1) BKP_READ(0)==0x5A5A?        → 强制升级（APP 主动触发）→ upgrade_mode
├─ 2) param.boot_state==UPGRADE?   → 运行时 OTA 下载完成 → upgrade_mode
├─ 3) param.boot_state==PENDING?
│     ├─ boot_count>=3 → 回滚（嘟-嘟）
│     ├─ 否则 count++ 持久化 → RUN 有效? → 跳 APP : 回滚
├─ 4) param.boot_state==RECOVERY?  → 进升级模式（有意妥协：不砖机）
├─ 5) 正常模式：
│     ├─ RUN 魔数+向量有效? → 跳 APP
│     ├─ 无效? → 外部备份有效? → 恢复并跳 APP
│     └─ 再无 → 进升级模式
```

## 五、升级流水线 boot_apply_download（boot_app.c:344-500）

```
VERIFY → BACKUP → ERASE → DECRYPT-WRITE → VECTOR-CHECK → COMMIT → CLEANUP → REBOOT
  ① 探测外部 ota_dl 包（ota_source.c）
  ② security_verify_and_decrypt：
     · ECC 签名验证（uECC）· SHA256 摘要 · AES-CTR 解密
     · 版本 ≥ 当前 + 防重放（last_build_no 单调）· 密钥由设备 UID 派生
  ③ 备份当前 RUN → 外部 img_lib（失败=中止升级复位回 APP）
  ④ 擦除内部 RUN 区（按扇区喂狗）
  ⑤ AES-CTR 边解密边写 Flash（UID 派生 32B 密钥 + 12B IV + 4B 计数器）
  ⑥ 写后向量校验（SP/PC 边界严格，同 boot_check_app_valid）
  ⑦ 写魔数+版本 → 置 PENDING（双次保存，失败宁停勿损）
  ⑧ 失效全部断点续传会话槽 + 擦除外部下载槽（防重放/防 stale-resume）
  ⑨ BKP 清标志 → 复位进入新固件（APP 侧启动确认后清 PENDING）
```

## 六、设计亮点（面试/复盘级素材）

1. **裸跳板**（:179-197）：`naked` 汇编 `msr msp → dsb/isb → bx`——切栈后不再有任何 C 尾声弹栈，从根上杜绝越界 HardFault
2. **向量校验双保险**：跳转前/写入后共用 `boot_vector_valid`，SP 严格 < SRAM/CCM 末端+1（杜绝越界一格）
3. **UID 派生密钥**（security.c:48）：一机一密，防固件被搬到别的板子
4. **防重放闭环**：`last_build_no` 单调递增，旧包/同版本包无法重放
5. **自愈哲学**（boot_abort_apply:324）：Flash 级失败不 halt——参数归一 + 复位，由状态机自动走"魔数无效→外部备份修复"
6. **断电注入测试**（POWERLOSS_TEST_STAGE:21）：编译期开关在 BACKUP/ERASE/WRITE/COMMIT 后模拟断电，验证每个阶段的恢复路径
7. **升级状态广播**（boot_status_send:96）：UART1 发 HOSTLINK 0x0C 帧，上位机实时可视化 7 阶段进度；波特率临时切 921600 发完恢复
8. **蜂鸣器旋律即进度**（:60-81）：滴=工作、嘟=提交、三短=失败、两长=回滚——无屏也能听出状态
9. **宁停勿损**（:465-469）：PENDING 参数保存失败 → 死循环喂狗等复位，绝不无保护上线
10. **YMODEM 按 4KB 扇区边界切分写外部 Flash**（:249-280）：不依赖"帧长整除扇区"隐式不变量

## 六点五、OTA 包格式与安全校验链（security.c 精读，194 行）

**固件包结构（外部 ota_dl 槽）**：
```
[ ota_header_t ] [ AES-CTR 加密的固件体 ] [ ECDSA 签名 64B ]
  魔数 0x4F5441FE · chip_id · build_no · firmware_size · version · aes_iv(12B) …
```

**verify_and_decrypt 六道关**（security.c:138-194）：
```
① 魔数匹配       0x4F5441FE（防垃圾数据）
② 芯片绑定       header.chip_id == DBGMCU->IDCODE（防跨芯片烧录）
③ 防重放         build_no > 参数区 last_build_no（严格递增）
④ 尺寸防护       firmware_size ≤ APP_SIZE（防溢出）
⑤ SHA256 校验    流式计算 header+body → 与签名比对
⑥ ECDSA 验签     secp256r1，新公钥失败再试 LEGACY 公钥（密钥轮换过渡期）
   版本防回滚     header.version ≥ 当前版本
```

**密钥体系**：
- 签名：私钥离线保管不入库，板载双公钥（新+LEGACY，轮换过渡后移除）
- 加密：`AES_KEY = SHA256(UID[12B] || "OTA-AES-KEY-V1"[15B])` —— 一机一密
- 解密：AES-256-CTR 流式（256B 分块边解边写，每块喂狗，security.c:94-124）

## 七、待精读清单（下一课）

- [x] `security.c`：ECC 验签 + AES-CTR 解密 + 防重放（已完成，见本笔记 六点五）
- [x] `ota_backup.c` / `ymodem.c` / `esp_flash.c` / `boot_param.c` / `flash_if.c` / `boot_err.c` 等全部自研服务（已完成 → 见 `02_boot_stage1_存储与传输层精读.md`）
- [ ] `tests/test_ymodem.c`：主机侧如何单测 BOOT 协议
- [ ] `uECC.c` / `aes.c` / `my_sha256.c`：加密三件套原理级扫读

## 八、自测题（读完笔记后自答）

1. 为什么跳转 APP 时必须用裸汇编而不是普通 C 函数调用？（提示：栈切换时序）
2. PENDING 状态在什么情况下会触发回滚？回滚源在哪里？回滚后 boot_count 怎么变？
3. 防重放是怎么实现的？为什么升级成功后要擦外部下载槽？
4. 如果升级写到一半断电（WRITE 后），复位后会发生什么？（提示：自愈路径）
5. BKP_DR1==0x5A5A 和 param.boot_state==UPGRADE 两条升级触发路径的区别？
