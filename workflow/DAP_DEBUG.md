# DAP 硬件调试手册（dap_debug.py）

> 排查 BUG 的第一原则：**用 DAP 读寄存器 / 断点定位，而不是反复加串口
> 打印、反复烧录**。本工具让 Codex 与人都能以“硬件级”视角直接观察
> 内核、内存与外设的实时状态。

## 1. 能力总览

| 能力 | 命令 | 说明 |
| --- | --- | --- |
| 暂停/恢复/复位 | `halt` / `resume` / `reset` | 单次会话；断开会话后内核自动恢复运行 |
| 读内核寄存器 | `reg pc sp lr r0..r12 msp psp` | 自动符号化 PC |
| 读内存/外设寄存器 | `read GPIOB_ODR` / `read 0x40020414 2` | 支持外设寄存器名、符号名、裸地址 |
| 写寄存器 | `write ADDR VALUE` | 危险操作，仅在明确需要时使用 |
| 故障解码 | `fault` | CFSR/HFSR/BFAR/MMFAR + MSP/PSP 异常栈帧 + 崩溃点符号 |
| 栈回溯 | `stack --depth 16` | 逐字展开栈，flash 值自动符号化 |
| 断点探测 | `bp 符号 --wait 1500` | 设断点→运行→等待→报告是否命中→自动清理 |
| 交互式调试 | `debug` | 持久会话：halt/step/bp 状态跨命令保持 |
| 地址↔符号 | `sym 0x0803BD92` / `sym 函数名` | 双向解析 Keil map |
| 外设布局 | `periph [CAN1]` | 查看已收录外设及其寄存器地址 |

## 2. 环境要求

- 开发板已上电，CMSIS-DAP 探针已连接（绿灯）。
- OpenOCD：`D:\GIT-SPACE\D00\tools\xpack-openocd-0.12.0-7\bin\openocd.exe`。
- 符号表：读取 `APP\APP\MDK-ARM\APP\APP.map` 与 `BOOT.map`（构建后自动更新）。
- **DAP 是单连接设备**：同一时刻只能有一个 OpenOCD 会话，严禁并行调用本工具。

## 3. 快速上手

```powershell
# 看当前执行位置（最有用的第一条命令）
D:\Python\python.exe D:\GIT-SPACE\D00\workflow\dap_debug.py pclist

# 读外设寄存器
D:\Python\python.exe D:\GIT-SPACE\D00\workflow\dap_debug.py read GPIOB_ODR
D:\Python\python.exe D:\GIT-SPACE\D00\workflow\dap_debug.py read I2C1_SR1
D:\Python\python.exe D:\GIT-SPACE\D00\workflow\dap_debug.py read USART3_CR1

# 批量读内存（4 字节对齐的连续字）
D:\Python\python.exe D:\GIT-SPACE\D00\workflow\dap_debug.py read 0x20000000 16

# 解码故障
D:\Python\python.exe D:\GIT-SPACE\D00\workflow\dap_debug.py fault
```

## 4. 典型排查场景

### 4.1 系统卡死 / 崩溃后无日志

```powershell
# 1) 看 CPU 现在停在哪（符号化）
dap_debug.py pclist
# 2) 解码硬件故障（含异常栈帧与崩溃点）
dap_debug.py fault
# 3) 栈回溯找调用链
dap_debug.py stack --depth 32
```

`fault` 输出要点：
- `CFSR` 非零位说明故障类型（总线错误 / 用法错误 / 栈错误等）；
- `BFAR`/`MMFAR` 是出错地址，若在 flash 会显示对应符号；
- `MSP/PSP exception frame` 中的 `PC` 即崩溃点；
- CFSR/HFSR 全 0 表示**当前无活动故障**（寄存器为历史残留），应结合
  `pclist` 判断是否只是逻辑死循环。

### 4.2 排查外设"为什么没工作"

外设问题 90% 是时钟没开、引脚模式错、使能位没置：

```powershell
# 时钟树：检查对应总线使能位
dap_debug.py read RCC_AHB1ENR   # GPIO 时钟
dap_debug.py read RCC_APB1ENR   # I2C/USART2/3/CAN 时钟
dap_debug.py read RCC_APB2ENR   # USART1/SPI1 时钟

# GPIO 状态：MODER 模式 / ODR 输出电平 / IDR 输入电平
dap_debug.py read GPIOB_MODER
dap_debug.py read GPIOB_IDR
dap_debug.py read GPIOB_ODR

# 外设自身状态
dap_debug.py read I2C1_SR1      # 标志位（BUSY/ADDR/TXE/RXNE）
dap_debug.py read CAN1_ESR      # CAN 错误状态（LEC/BOFF）
dap_debug.py read USART3_SR     # 串口状态
dap_debug.py read TIM2_CNT      # 定时器计数是否在走
```

### 4.3 断点探测："这段代码到底跑没跑？"

对符号设置断点，工具会运行等待并报告是否命中，然后自动清理、恢复系统：

```powershell
# 2 秒内 vTaskSwitchContext 是否被调用
dap_debug.py bp vTaskSwitchContext --wait 2000

# 自定义等待时间（毫秒）
dap_debug.py bp 0x0803BD60 --len 2 --wait 5000
```

命中输出示例：
`HIT bp@vTaskSwitchContext: pc=0x08069FE4 <vTaskSwitchContext+0x0>`

未命中说明该路径未执行（或等待窗口太短），可加大 `--wait`。

### 4.4 交互式逐步调试（最强大模式）

```powershell
dap_debug.py debug
```

进入持久会话后 halt/断点/单步状态**跨命令保持**，等同 Keil 在线调试：

```
dap> bp 0x08069FE4 2 hw      # 设硬件断点（注意 OpenOCD 语法必须带长度）
dap> resume                  # 继续运行
dap> sleep 1200              # 等 1.2 秒（OpenOCD 命令，期间断点命中会暂停）
dap> reg pc                  # 命中后 PC 停在断点
dap> step                    # 单步一条指令
dap> reg r0 r1               # 看函数参数
dap> rbp all                 # 清除所有断点
dap> resume                  # 恢复系统运行
dap> quit                    # 退出（自动 shutdown）
```

常用 OpenOCD 命令：`reg`、`mdw/mdh/mdb`、`bp <addr> <len> hw`、`wp <addr> <len> rw`、
`step`、`resume`、`halt`、`reset`、`rbp all`、`shutdown`。

### 4.5 追踪函数调用（栈上的返回地址）

```powershell
dap_debug.py stack --depth 32
```

flash 范围内的字会自动标注符号（如 `<modules_init+0x39>`），可据此重建调用链。

## 5. 安全约定（重要）

1. **看门狗自动冻结**：发布构建（`APP_DEBUG_MODE=0`）IWDG 开启，普通 halt 超过
   看门狗周期会复位整板。本工具在每个会话自动写 `DBGMCU_CR=0x1F`，halt 期间
   冻结 IWDG/WWDG/定时器，**断点挂多久都不会被复位打断**。该位只在 halt 时生效，
   正常运行不受影响。
2. **单次会话自动恢复**：`pclist`/`read`/`fault`/`stack` 等命令采集完数据即
   `resume` 并断开，板子继续正常运行，不会停留在挂起态。需要持续挂起排查时
   使用 `debug` 交互模式。
3. **DAP 单连接**：绝不并行执行两个命令（会互斥超时）。
4. **写操作谨慎**：`write` 直接改硬件状态，确认地址与值无误后再执行；建议只写
   调试寄存器或明确已知的 RAM 变量。
5. **Thumb 位已屏蔽**：map 符号地址含 bit0=1（Thumb 标记），工具已自动屏蔽，
   `bp`/`read` 用偶地址，无需手动处理。
6. **调试后确认系统恢复**：交互会话退出前执行 `resume` 或 `reset`；若板子行为
   异常（如停在 BOOT），先 `reset` 一次再观察。

## 6. 与 AI 工作流的配合

- 复现 BUG 后**第一动作**是 `pclist` + `fault`，用证据定位，而不是盲加打印。
- 怀疑某个路径未执行 → `bp` 探测；需要逐步观察 → `debug` 交互会话。
- 寄存器证据要记录到 `ENGINEERING_LOG.md`（含地址、值、符号化结果）。
- 涉及协议/时序/并发的问题，按 `docs\ISSUE_POSTMORTEM_TEMPLATE.md` 复盘。
