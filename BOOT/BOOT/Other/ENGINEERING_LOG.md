# BOOT 工程日志 · 问题复盘与联动契约

> 记录 BOOT 与 APP 联合调试中发现的问题、修复与关键约定，供复盘学习。
> APP 侧对应日志见 `APP/Doc/ENGINEERING_LOG.md`。

## 1. 2026-08 联动检查与修复

### 1.1 BOOT.sct 的 RAM 声明与硬件不符（已修复）
- **现象**：`MDK-ARM/BOOT/BOOT.sct` 中 `RW_IRAM1 0x20000000 0x00030000`（192 KB），
  但 STM32F407 主 SRAM 仅 128 KB（`0x20000000~0x2001FFFF`）。
- **根因**：历史遗留的复制粘贴值；`<Cpu>` 行与 `CheckAppValid` 的栈指针上限
  （`0x20020000`）都是正确的 128 KB，仅分散加载文件写错。
- **解决**：`0x00030000` → `0x00020000`，并补充注释。
- **验证**：Keil 全量重编译 0 Error / 0 Warning，BOOT.bin 正常生成。

### 1.2 直烧 APP 魔数机制说明（与 APP 联动）
- BOOT 通过 `0x0805FFF8` 的 `0x4F54412E` 判断 APP 有效；
- OTA 流程由 BOOT 在验证通过后写入魔数与版本；
- 直接烧录 APP 时须先用 `APP/Script/append_app_magic.py` 生成带魔数的完整镜像，
  否则 BOOT 会进入升级模式（详见 APP 日志 8.2）。

## 2. 与 APP 的联动契约（勿轻易改动）

| 项 | 值 |
| --- | --- |
| BOOT 区 | 64 KB @ `0x08000000` |
| APP 区（RUN） | 320 KB @ `0x08010000`（末尾 8B = 魔数 + 版本） |
| BACKUP 区 | 256 KB @ `0x08060000`（回滚源，尾部 8B 独立有效性） |
| Download 区 | 256 KB @ `0x080A0000`（尾部 24KB 会话槽区，固件包 ≤232KB） |
| APP 有效性魔数 | `0x4F54412E` @ `0x0805FFF8` |
| APP 版本号 | @ `0x0805FFFC` |
| 升级请求标志 | `RTC_BKP_DR1 == 0x5A5A` |
| OTA 包头魔数 | `0x4F5441FE` |
| 跳转目标 | `0x08010000`，跳转前清外设/中断并设 VTOR |

## 3. 待办

- 上板回归：升级请求 → OTA → 校验 → 跳转 → 运行，全链路验证。

---

## 4. 分层重构（2026-08）

### 4.1 目录分层
- 原 `Core/Src`、`Core/Inc` 业务代码平铺 → 参照 APP 分层拆解：
  - `BootApp/`   应用层：启动流程 / APP 校验 / 跳转 / 升级状态机
    （从 main.c 抽离，main.c 由 379 行瘦身到 162 行，仅保留 CubeMX 骨架）；
  - `BootServices/` 服务层：ymodem / ymodem_port / flash_if / security /
    aes / my_sha256 / uECC / crc32 / fifo；
  - `Config/`   配置集中：boot_config.h；
  - `Core/`     仅保留 CubeMX 生成文件（main/gpio/dma/iwdg/rtc/usart/it/msp/system）。
- Keil 工程与 include 路径同步更新。

### 4.2 构建与测试
- 新增 **GCC 交叉编译目标**（`CMakeLists.txt` + `Core/Startup/BOOT.ld` +
  `cmake/toolchain-stm32f4.cmake`），独立产出 BOOT.bin；
- 新增 **主机单元测试**（`tests/test_services.c`：CRC-32 标准向量 +
  FIFO 容量/环绕），CMake host 目标。

### 4.3 顺手修复
- `my_sha256.c` 缺失末尾换行（Keil #1-D 告警）；
- `flash_if.c` 未使用的 `flash_program_word` 静态函数（Keil #177-D 告警）；
- `security.h` 的 ARMCC `#pragma diag_suppress` 增加编译器条件守卫（GCC 兼容）。

### 4.4 验证
- Keil 全量重编译：0 Error / 0 Warning；
- GCC 固件交叉编译：通过（text≈44.7 KB < 64 KB Flash）；
- 主机单元测试：全部通过。

### 4.5 192 KB RAM“幽灵”根因（重要复盘点）
- **现象**：BOOT.sct 手改为 128 KB 后，每次 `UV4 -r` 重建都被覆盖回 192 KB
  （`RW_IRAM1 0x20000000 0x00030000`）。
- **排查过程**：改 `<IRAM>`、`<ScatterFile>`、`OCR_RVCT5`、设只读均无效——
  Keil 每次重建都重新生成 sct。
- **真正根因**：`BOOT.uvprojx` 的 **`OCR_RVCT9`**（散列模式 RAM 区域）被配置成
  `0x20000000 / 0x30000`（192 KB），Keil 生成器按它生成 sct。
  STM32F407 主 SRAM 仅 128 KB（0x20000000~0x2001FFFF）。
- **解决**：`OCR_RVCT9` 改为 `0x20000000 / 0x20000`（Type=1），
  重建后 sct 正确生成 128 KB 且不再被覆盖。
- **验证**：BOOT `-r -b` 0/0，sct 保持 `RW_IRAM1 0x00020000`。

### 4.6 烧录与工具链优化（2026-08）
- **BOOT 下载擦除模式**：`CMSIS_AGDI` 的 `-FO` 从 23（Full Chip Erase）改为 15
  （Erase Sectors）——烧 BOOT 不再整片擦除，APP 与有效性魔数得以保留；
  实测日志由 `Full Chip Erase Done` 变为 `Erase Done`，烧录后自动运行且 APP 正常跳转。
- **OTA_Tool 迁移**：升级工具本质是上位机，已整体迁移至 `HOST/OTA_Tool/`
  （加密打包 + YMODEM 发送，COM 口 115200），与 VLink_Debugger 并列。
