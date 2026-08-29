# D00 源码研学 · 笔记 02 —— BOOT 存储层与传输层精读

> 精读对象：BootServices 剩余自研模块（约 1,600 行）：
> `ymodem.c`(429) · `ymodem_port.c`(56) · `fifo.c`(28) · `esp_flash.c`(260) ·
> `flash_if.c`(201) · `ota_backup.c`(189) · `boot_param.c`(114) · `ota_source.c`(21) ·
> `crc32.c`(79) · `boot_err.c`(152)
> 配套：笔记 01（启动状态机 + 升级流水线）· AGENTS.md 分区约定

---

## 一、BootServices 分层地图（自研 4,397 行 = 10 个模块）

```
                    ┌─────────────────────────────────────────┐
  业务层             │  boot_app.c        启动状态机 + 升级主流程   │
                    └──────────┬──────────────────────────────┘
                               │ 调用
  服务层        ┌──────────────┼───────────────┐
               ▼              ▼               ▼
        security.c       ota_backup.c     boot_param.c
        安全链+解密        外部备份/回滚      参数区(双份+CRC)
               │              │               │
               │        ota_source.c     boot_err.c
               │        读源抽象           崩溃诊断
               │
  传输层        ▼
        ymodem.c + ymodem_port.c + fifo.c
        YMODEM 状态机 / UART 移植 / 环形缓冲
               │
  存储层        ▼
        esp_flash.c          flash_if.c
        外部 W25Q128(SPI1)    内部 Flash 擦写
```

**三条数据路径**（都汇入存储层）：
1. **运行时 OTA**：APP 经 HOSTLINK 下载 → 外部 `ota_dl` 槽 → 复位 → BOOT 校验解密 → 写内部 RUN
2. **YMODEM 兜底**：上位机串口直传 → 同样写入外部 `ota_dl` 槽 → 与路径 1 共用读源
3. **备份/回滚**：内部 RUN ⇄ 外部 `img_lib` 槽（升级前备份 / 失败后恢复）

---

## 二、外部 Flash 分区与布局（esp_flash.h:17-25）

| 外部槽 | 基址 | 大小 | 用途 |
| --- | --- | --- | --- |
| `ota_dl` 下载区 | 0x000000 | 1MB（分区 2MB，单槽 1MB） | 固件包暂存（头+832KB+签名 ≤ 1MB） |
| `img_lib` 镜像库 | 0x200000 | 896KB（=14×64KB 块） | 备份头 4KB + RUN 全量 832KB + 余量 |

`img_lib` 内部布局（ota_backup.h:9-11）：
```
[0x0000, 0x1000)  备份头 32B（独立 4KB 扇区，防数据覆盖向量表）
[0x1000, ...)     RUN 全量镜像 832KB
```

---

## 三、esp_flash.c —— 外部 NOR 驱动（W25Q128，260 行）

**硬件**：SPI1 主模式 8bit **42MHz**（BR=0，APB2/2），PB3/4/5=SCK/MISO/MOSI(AF5)，PB14=CS。
寄存器级阻塞轮询（TXE/RXNE 带超时），**BOOT 无 OS，长操作逐块喂 IWDG**。

**命令集**（esp_flash.c:16-22）：`0x03` 读 · `0x02` 页写(256B) · `0x20` 扇区擦(4KB) · `0xD8` 块擦(64KB) · `0x05` 读状态 · `0x06` 写使能 · `0x9F` JEDEC ID

**关键实现**：
| 函数 | 要点 |
| --- | --- |
| `EspFlash_Init` (:97-136) | 开时钟→GPIO 复用→SPI1 主模式→**JEDEC 探测**：ID 必须 == `0xEF 0x40 0x18`（Winbond W25Q128），否则返回 false（外部 Flash 离线→备份功能禁用但升级仍可走） |
| `EspFlash_Write` (:174-205) | **页编程自动跨页**：按 256B 页边界切 chunk，每页先 `0x06` 写使能再写，写后 `esp_wait_busy` 等 WIP 清零 |
| `esp_wait_busy` (:76-94) | **毫秒级超时（5s）而非迭代计数**——注释明确：迭代次数与时钟频率耦合，最坏情况误判失败；等待中喂狗 |
| `EspFlash_EraseRange64` (:246-259) | 64KB 块粒度擦除大区域（备份/清槽提速），逐块喂狗 |
| `EspFlash_HasPackage` (:262-287) | 只读槽头 16B 探测：magic==0x4F5441FE 且 `firmware_size + 头 + 签名 ≤ 1MB`，返回包总长 |

---

## 四、flash_if.c —— 内部 Flash 擦写（201 行）

**扇区映射**（:13-26）：F407 1MB = 12 扇区（0-3×16KB，4-11×128KB）。`flash_get_sector` 是**唯一实现**，boot_param.c 的 `hal_erase_sector` 复用它（注释强调"禁止私抄映射链"）。

**`flash_erase`（寄存器级，:35-86）——三个精髓**：
1. **`.ramfunc` RAM 喂狗**（:6-9）：`__attribute__((section(".ramfunc")))` 把喂狗函数放 RAM —— 擦除期间 CPU 从 Flash 取指会 stall，**必须从 RAM 执行**
2. **BSY 超时诊断**（:59-69）：`guard > 3000万次(~0.18s@168MHz)` 判 BSY 卡死，显式失败返回——"绝不让未真正擦除的扇区被当作成功继续写"
3. **错误标志全清**（:48-50）：清 PGSERR/PGPERR/PGAERR/WRPERR/SOP —— **RDP 解除后可能残留 SOP/OPTERR，不清会导致 HAL 擦除失败**

**`flash_write`（HAL 字编程，:91-146）**：`__disable_irq()` 关中断 → 前导非对齐字节用**读-改-写**（读旧 word、改字节、整字写回）→ 整字 4B 循环 → 尾部同前导 → 每字喂狗。

**`flash_copy_raw`（寄存器级直拷，:151-230）**：内部 Flash 区对拷（APP 内部旧 BACKUP→RUN 的遗留路径）。
⚠️ **经典坑**（:160-163）：**必须设 `PSIZE=WORD`（x32 编程）**——复位后 PSIZE=00 是 x8 编程，直接写 32 位字会触发 PGPERR。同时清 SNB 残留（上次扇区擦除留下的扇区号）。

---

## 五、YMODEM 协议状态机（ymodem.c 429 + ymodem_port.c 56 + fifo.c 28）

### 5.1 协议选型：**YMODEM-1K 变体，但帧校验用 CRC32 而非标准 CRC16**
`YMODEM_PACKET_SIZE=1024`（STX 帧）/ `FILE_INFO_SIZE=128`（SOH 帧），帧格式：
```
[SOH 0x01 | STX 0x02] [seq] [~seq] [data...] [CRC32 4B 小端]
```
> 注意：标准 YMODEM 用 CRC16-XMODEM；本项目自定为 **CRC32**（帧内校验 + 整文件流式 CRC32 双重校验）。上位机 `OTA_Tool` 必须配套实现（后续 HOST 精读验证）。

### 5.2 传输时序（完整握手）
```
BOOT                   上位机
 │  'C'×5 (128B 模式邀请)
 ├──────────────────────►
 │    (文件信息帧: 文件名\0 大小HEX 空格 CRC32HEX)
 │◄──────────────────────
 │  ACK + 'C' (1K 模式邀请)
 ├──────────────────────►
 │◄── 数据帧 #1 (1024B) ──
 │  ACK
 ├──────────────────────►
 │◄── ... #N ...
 │◄── EOT ──► NAK
 │◄── EOT ──► ACK + 'C'   ← 双 EOT 握手，进入结束帧
 │◄── 结束帧 (SOH,seq0,文件名空) ──► ACK
 │ 整文件 CRC32 比对 → 完成
```

### 5.3 状态机（7 态）
```
INIT ──'C'×5──► WAIT_FILE_INFO ──SOH 头──► RX_FRAME ──解析成功──► DATA_PHASE
                  ▲                          │                    │
                  │◄───────── 帧错/超时 NAK ──┘                    │
                  │                                                ▼
                  │◄────────────── EOT ──NAK── EOT_PHASE ◄── 数据帧完成
                  │                       │ 结束帧
                  │                       ▼
                  └──────────► COMPLETE（文件 CRC32 终检）
                              ERROR（CAN×5 取消 / 超限 / 解析失败）
```

### 5.4 健壮性细节（面试素材）
| 机制 | 实现 | 位置 |
| --- | --- | --- |
| 重传 | NAK 后重发，`YMODEM_MAX_RETRY=10` 超限 CAN×5 取消 | ymodem.h:23, ymodem.c:176-183 |
| 分相超时 | 文件信息 3s / 数据 5s / EOT 3s，非阻塞轮询 tick | ymodem.c:24-27 |
| 帧校验 | seq+~seq==0xFF 且 CRC32 匹配，否则 NAK | :302-316 |
| **重复帧** | seq==前序 → ACK 但丢弃数据（防重发歧义） | :384-386 |
| **写越界防护** | `write_addr+len > flash_end` → CAN 取消（双保险：文件大小检查+写前检查） | :333-339, :359-363 |
| 介质抽象 | `ctx->write_fn` 回调写目标——**状态机不感知物理介质**（方案B=外部 Flash） | ymodem.h:47-50 |
| 文件大小 | `strtoul(size_str, NULL, 16)` **十六进制解析**（YMODEM 规范） | :462 |
| 边收边写 | 收到一帧即写 Flash，不整包缓存（1KB 帧缓冲） | :368-374 |
| 喂狗 | 每帧循环 `ymodem_feed_watchdog()` | :103 |

### 5.5 UART 移植层（ymodem_port.c）
- **RXNE 中断 + 环形 FIFO**：`ymodem_uart_isr_handler` 在 USART1 中断里收字节入 FIFO（fifo.c 28 行：head/tail 环形，满则丢）
- **先停 DMA 再初始化**（:18-22）：`HAL_UART_AbortReceive` + 关 IDLE 中断，把 UART1 从 APP 时代的 DMA/IDLE 模式切回 RXNE 中断模式（**BOOT/APP 复用同一条 UART1，切换必须干净**）
- 115200 8N1；`ymodem_read_byte(timeout)` 轮询 FIFO + 超时，**等待期间喂狗**

---

## 六、ota_backup.c —— 外部备份/回滚（189 行）

**备份头**（ota_backup.h:26-33，`#pragma pack(1)` 32B）：
```
magic 'BKP1'(0x31504B42) | app_size(=APP_SIZE) | build_no(来自 APP_VERSION_ADDR) | crc32 | reserved[4]
```

**`OtaBackup_Save`（RUN→外部，:86-154）——全量快照 + 写后读回 100% 校验**：
```
① 64KB 块擦除 896KB（逐块喂狗）
② 写备份头（build_no 从 RUN 尾部版本号读）
③ 512B chunk 循环：内部 Flash 内存直读 → 外部写（BKUP_CHUNK=512 栈友好，静态缓冲）
④ 读回逐块 memcmp 比对（s_cmp 静态缓冲）
⑤ 读回头再校验 → 完成
```
**任何一步失败 → 返回 false → 升级中止复位回 APP**（"备份失败宁可不升"）。

**`OtaBackup_Restore`（外部→RUN，:157-198）**：
```
① 校验头（magic + app_size==APP_SIZE + CRC）
② flash_erase 擦 RUN 全区
③ 512B chunk：外部读 → flash_write 内部写
④ 补写尾部魔数 0x4F54412E @0x080DFFF8 + 版本 @0x080DFFFC（备份数据区不含尾部 8 字节有效性）
```

**`OtaBackup_Clear`（:201-207）**：升级成功后擦除整槽——**防重放 + 防 stale 备份被误恢复**。

---

## 七、boot_param.c —— 参数区（114 行）

**存储策略：同一扇区（扇区 11）内双份块**（PARAM_BASE + 0 / +1024）：
- **读**（:80-95）：两都有效取第一份；单有效取有效份；都无效回默认 NORMAL
- **写**（:97-129）：整扇区擦除 → 写两份 → **回读验证**（注释：排查"假写成功"）

**三大陷阱注释（都是血泪史）**：
1. **CRC 只算到 `crc32` 字段之前**（:38-56）：若把 crc32 自身纳入计算，save 基于旧值算、写入新值后 load 用新值重算必然不等 → 参数区永远校验失败（PENDING 不持久化！）
2. **早期擦参数扇区会 Flash BSY 卡死**（:14-17）：不在 BOOT 启动早期调用 `hal_erase_sector`，统一在进入升级模式后（延迟执行）使用
3. **CRC 实现禁止替换**：此处为"无 final-xor 的位算法"，与 crc32.c 查表实现差一次 0xFFFFFFFF 异或——参数区已有存量数据依赖本实现，换了全部失效、防重放退化

---

## 八、⭐ 全仓三个 CRC32 并存（兼容性教训）

| 实现 | 算法 | final-xor | 用途 |
| --- | --- | --- | --- |
| `crc32.c` | 查表 | **有**（^0xFFFFFFFF） | YMODEM 帧校验 + 整文件流式校验 |
| `bkup_crc32`（ota_backup.c:30-41） | 逐位循环 | 无 | 备份头校验 |
| `boot_param_crc`（boot_param.c:38-56） | 逐位循环 | 无 | 参数区校验 |

**为什么不能统一？** 两个持久化结构（备份槽/参数区）的存量数据是用各自旧算法算的 CRC。换实现 = 所有存量数据校验失败 = 备份全部失效、PENDING 状态丢失（防重放退化）。源码里两处显式警告"禁止替换"。
**教训**：持久化格式的校验算法一经发布就是 ABI，宁可代码丑一点，不可动存量数据。

---

## 九、boot_err.c —— BOOT 崩溃诊断（152 行）

**设计原则**（boot_err.h:6-15）：无 RTOS 不回溯任务栈；**统一 fault 入口打印诊断后软复位，绝不死循环**；崩溃摘要持久化 BKP reg 16-19（与 APP err_mgr 的 reg 1-15 隔离，reg 0 留给 OTA 标志）；不做防抖锁定（BOOT 必须能自恢复）。

**`Boot_ErrFaultEntry`（:73-132）**：
1. `__disable_irq()` + **裸寄存器中止 USART2 TX DMA**（fault 上下文不依赖 HAL/printf，防中断上下文阻塞）
2. 直接写 `USART2->DR` 输出诊断报告：源（NMI/HardFault/...）、**CFSR 原因解码**（DIVBYZERO/UNALIGNED/INVPC/INVSTATE/UNDEFINSTR/PRECISERR/IMPRECISERR/IBUSERR/DACCVIOL/IACCVIOL）、PC/LR/xPSR/EXC_RET/R0-R3/MSP/PSP
3. 摘要持久化 BKP reg 16-19：`magic 'BTR1'` | `src+seq(自增)` | `PC` | `bkp_crc`（自创简单混淆校验，防残留垃圾误报）
4. `NVIC_SystemReset()` 软复位

**`Boot_ErrReportLast`（:135-152）**：启动时校验 magic+CRC，打印 `[BOOT-CRASH] Previous BOOT fault recovered: ...`——**崩溃自愈闭环**。

---

## 十、设计亮点汇总（面试/复盘级）

1. **RAM 喂狗**：`.ramfunc` 段 + 擦除期间从 RAM 执行——Flash 擦写时取指 stall 的教科书解法
2. **介质抽象双杀**：YMODEM 用 `write_fn` 回调、升级用 `ota_source_t` 读源抽象——业务层完全不感知 SPI/物理介质，可单测可换介质
3. **写后读回校验**：备份 832KB 逐块 memcmp 比对 + 头回读——"写成功"必须被证明
4. **分层超时**：YMODEM 三档超时 + Flash BSY 计数超时 + SPI 毫秒超时——每一层都不允许永久卡死
5. **BSY 超时显式失败**：绝不把"未真正擦除"当成功继续写（防半擦状态写坏数据）
6. **PSIZE=WORD 坑**：寄存器级编程必须显式 x32，复位默认 x8 直接写 32 位字 = PGPERR
7. **CRC 覆盖范围纪律**：crc32 字段绝不算自己；持久化算法一经发布不可替换
8. **双份参数区**：同扇区双份 + 读选优 + 写回读验证——单点损坏自动恢复
9. **BKP 摘要自校验**：崩溃信息用 magic+CRC 防误报，软复位自愈不死循环
10. **每块喂狗哲学**：任何长操作（擦/写/拷/传）都以块为单位喂狗 + IWDG 16.4s 总兜底

## 十一、待读清单（下一课）

- [x] `ota_backup.c`：外部备份/回滚（本轮完成）
- [x] `ymodem.c` + `ymodem_port.c`：YMODEM 状态机与移植层（本轮完成）
- [x] `esp_flash.c`：W25Q 驱动（本轮完成）
- [x] `boot_param.c`：双份参数读写（本轮完成）
- [x] `flash_if.c` / `boot_err.c` / `crc32.c` / `fifo.c` / `ota_source.c`（本轮完成）
- [ ] `tests/test_ymodem.c`：主机侧如何单测 BOOT 协议
- [ ] `uECC.c`(1475, 第三方) / `aes.c`(484, TinyAES) / `my_sha256.c`(96)：加密三件套原理级扫读（只读接口与调用关系）
- [ ] `BootApp` 其余文件（boot_loader / boot_upgrade 拆分？）+ `Core` 启动（startup/时钟/IWDG 配置）

## 十二、自测题（读完笔记后自答）

1. 为什么 `ram_feed_dog` 必须放 `.ramfunc` 段？如果不放，擦除期间会发生什么？
2. YMODEM 传输中收到重复帧（seq == 上一帧），BOOT 为什么 ACK 但不写 Flash？
3. `flash_copy_raw` 为什么必须设 `PSIZE=WORD`？复位后默认 PSIZE 是多少位编程？
4. 备份头为什么放在独立 4KB 扇区（0x1000 偏移），直接放 0x0 会怎样？
5. 三个 CRC32 实现为什么不统一？改成同一个会破坏什么？
6. `boot_param_crc` 为什么只算到 crc32 字段之前？算上自己会怎样？
7. YMODEM 与标准 YMODEM 的最大差异是什么？上位机需要配套什么？
8. 升级成功后的 `OtaBackup_Clear` 解决了什么问题（两个）？
