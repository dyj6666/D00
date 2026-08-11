# D00 统一注释规范

> 本规范是 **APP 固件 / BOOT 引导 / HOST 上位机 / workflow 脚本** 的统一注释标准。
> 目标：**看到注释就能知道代码在干什么**——不读实现也能跟上意图；
> 读实现时，关键语句的注释直接解释"为什么/做什么"。
>
> 配套：所有源码 UTF-8 编码、空格缩进；注释一律中文（协议名/寄存器名/枚举值除外），
> 每行不超过 100 列，行尾注释与代码之间至少空一格，优先对齐。

---

## 1. C 语言（.c / .h）

### 1.1 文件头 —— 统一横幅（每个源文件必须有）

格式固定为 78 列等宽框，四段内容：**职责 / 架构位置 / 核心流程 / 关键约束**。
某段无内容则整段省略，但框线不变。

```c
/* ================================================================
 * ota_agent —— 运行时 OTA：下载到 DOWNLOAD 区 + 启动确认
 *
 * 架构位置：APP 应用层；供 data_link / cmd_shell / OtaTcp / OtaHttp 调用
 * 核心流程：BEGIN -> DATA(240B/块) -> END -> 复位进 BOOT -> 启动确认成功
 * 关键约束：Flash 编程期间关中断；调用方必须顺序写、不跳块
 * ================================================================ */
```

头文件（.h）用同一格式，末尾追加 `#ifndef` 保护块注释：

```c
/* ================================================================
 * ota_transport —— 多协议 OTA 传输注册表（传输层抽象）
 *
 * 架构位置：APP 应用层；OtaMgr 登记 UART/TCP/HTTP/CAN，供命令层查询
 * 核心流程：OtaMgr_Init 注册 -> ota status 展示 -> 新传输 Register 即插即用
 * ================================================================ */
#ifndef OTA_TRANSPORT_H
#define OTA_TRANSPORT_H
...
#endif /* OTA_TRANSPORT_H */
```

### 1.2 区段分隔线

函数上方用虚线分隔，宽度随内容调整：

```c
/* ---------------- 内部状态与常量 ---------------- */
/* ---------------- 传输层接口 ---------------- */
```

### 1.3 函数注释 —— Doxygen 风格

每个非 static 函数、以及逻辑复杂的 static 函数都必须有块注释：

```c
/**
 * @brief  向下载区写入一块固件（严格顺序写）
 * @param  offset  本块相对固件起点的偏移，必须等于已收字节数
 * @param  data    块数据指针
 * @param  len     块长度，不得超过 OTA_CHUNK_MAX
 * @return 0=成功；1=非接收态；2=参数非法；3=Flash 写失败
 * @note   调用方须保证 offset 连续；失败后状态回到 IDLE
 */
```

简单函数可压缩为单行 Doxygen：

```c
/** @brief 获取当前 OTA 状态与进度（线程安全） */
```

### 1.4 语句级注释 —— 关键语句必须"看得懂"

**重要声明 / 赋值 / 调用：行尾注释，解释"做什么 / 为什么"。**
同段行尾注释右边界对齐（用空格，不用 Tab 混合对齐）。

```c
static volatile uint8_t ota_state = OTA_ST_IDLE;  /* 会话状态机，ISR 外读写 */
ota_total = size;                                 /* 记录本次固件总长，供收齐判定 */
ota_session_save(slot, ver, size, rx);            /* 每块持久化进度，断电可续传 */
```

**逻辑分支 / 循环 / 复杂表达式：上方独立一行注释。**

```c
/* 存在同版本同大小的有效会话 -> 不擦下载区，从断点续传 */
if (sess_ok && sess.version == version && sess.total == size) {
```

**宏 / 常量：定义处必须注释含义与约束。**

```c
#define OTA_CHUNK_MAX  240   /* 单块最大字节：与会话槽(240B/槽)严格配套 */
```

### 1.5 结构体 / 枚举

```c
typedef struct {
    uint32_t magic;      /* 魔数，识别有效会话 */
    uint32_t version;    /* 固件版本，防降级 */
    uint32_t received;   /* 已收字节，断点续传基准 */
    uint32_t crc32;      /* 覆盖 magic..received 的校验和 */
} ota_session_t;
```

---

## 2. Python（.py）

### 2.1 模块 docstring

```python
"""ETH TCP OTA 命令行工具 —— 与 HOSTLINK CLI 同构，走 :9020 帧协议。

流程：构建加密签名包 -> BEGIN -> DATA(240B/块) -> END -> BOOT 校验切换。
"""
```

### 2.2 类 / 函数 docstring

```python
class OtaWorker(QThread):
    """OTA 升级工作线程：hostlink / ymodem 两种模式。"""


def encrypt_and_sign(input_bin, output_bin, private_key_hex,
                     aes_key_hex, version=1, chip_id=0x413, build_no=1):
    """生成加密签名固件包。

    :param input_bin: 明文 APP.bin 路径
    :param output_bin: 输出加密包路径
    :return: 无（异常时抛出）
    """
```

### 2.3 语句级注释

```python
# 单设备自动分配；批量场景由调用方预分配，避免并发竞争
self.build_no = alloc_build_no(lib)
```

---

## 3. PowerShell（.ps1）

### 3.1 脚本头

```powershell
# ================================================================
# auto_ota —— HOSTLINK 串口 OTA 冒烟流水线
#
# 流程：构建包 -> COM13 上传 -> BOOT 阶段轮询 -> COM9 启动日志核验
# 用法：.\auto_ota.ps1 [-Version N] [-BuildNo N] [-Port COM13]
# ================================================================
```

### 3.2 函数 / 语句级

```powershell
function Test-BootLog {
    # 在启动日志中查找关键标记，返回核验结果对象
}
```

```powershell
$otaOk = ($r.ExitCode -eq 0) -and ...   # 下载阶段必须零错误
```

---

## 4. 红线

- 注释是**解释意图**，不是复读代码（禁止 `i++; /* i 自增 */` 这类废话）。
- 修改代码时必须同步更新其注释；注释与代码不一致视为 bug。
- 不写"TODO 未来再说"式悬空注释；确需 TODO 必须写明上下文与验收条件。
- 行尾注释右对齐；块注释不缩进进函数体时用 `/* ... */` 独占行。
- 编码必须 UTF-8（工程自检会校验），禁止引入全角空格或制表符混排。
