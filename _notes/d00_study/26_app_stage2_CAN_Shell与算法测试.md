# D00 源码研学 · 笔记 26 —— CAN Shell 适配器与算法库测试（Application 收官）

> 精读对象：`SystemServices/cmd_can.c`(119) · `Application/ctrl/test_ctrl.c`(812)
> **Application 自研代码至此全部精读完毕**（10,502 行，与 SystemServices 25,290 + Core 3,080 合计 APP 阶段完成）

---

## 一、CAN Shell 适配器（cmd_can.c）—— 第三传输通道

### 1.1 数据流（头注释 :6-9）
```
RX：BSP 回调(0x100) → 行帧组按序拼行 → Cmd_SessionFeed 分发
    （命令执行期间 LOG_Printf 自动路由回本适配器，输出零改动）
TX：ctx.out → 整段切帧（seq 递增，末帧 0x80）→ BSP_CAN_Send(0x101)
```

### 1.2 行帧规约（can_proto.h）
```
[seq 3bit | last 1bit | 保留 4bit][data ≤7B]
seq==0 新行组开始；last 置位 = 行组结束
乱序 → 整组丢弃防串行；超长行 → 丢弃重来
```

### 1.3 与 OTA-CAN 的对照
| 维度 | CAN Shell (0x100/0x101) | CAN OTA (0x200/0x201/0x210) |
| --- | --- | --- |
| 内容 | 命令文本行 | 二进制控制/数据帧 |
| 组帧 | 行帧（seq+last） | 240B 块拆分 |
| 会话 | Cmd_SessionFeed（统一框架） | ota_can 自有状态机 |
| 输出 | 切帧回传 | ACK 应答 |

**共同纪律**：seq 乱序整组丢弃（总线故障防护）；超长丢弃重来；严格 ID 过滤。

## 二、⭐ 算法库测试（test_ctrl.c，812 行）—— 55 个用例全覆盖

### 2.1 测试矩阵
| 族 | 用例数 | 代表断言 |
| --- | --- | --- |
| PID (14 变式) | 18 | `test_pid_antiwindup`（饱和后积分不爆）、`test_pid_autotune`（自整定收敛 Kp/Tu>0）、`test_pid_smith`（延迟补偿后无振荡）、fuzzy 规则表全格扫描 |
| KF (15 种) | 15 | `test_kf_1d_converge`（估计收敛 5.0±0.2）、`test_kf_generic_matches_1d`（**通用矩阵实现与标量结果一致**）、`test_infokf_matches_1d`、`test_kf_sqrt_matches_1d`（等价性验证）、`test_mahony_quat_norm`（四元数归一） |
| 滤波 (14 种) | 13 | `test_notch`（**50Hz 衰减 >20dB**）、`test_hpf_dc_reject`（直流完全抑制）、`test_debounce`（3 次确认翻转）、`test_sg_constant`（常数保真） |

### 2.2 设计亮点
1. **等价性测试**：通用 KF vs 标量 KF / 信息滤波 vs KF / 平方根 vs KF——**不同实现必须收敛到同一结果**
2. **频域验证**：陷波器注入 50Hz 正弦测衰减——算法正确性的物理验证
3. **全格扫描**：模糊规则表 5×5 全组合输出在 [-1,1] 且输出限幅内
4. **主机可跑**（无硬件依赖）：CI 中 `auto_hosttest.ps1` 执行（AGENTS.md 第 4 节）

## 三、⭐ APP 阶段总结（笔记 04-26，共 23 篇）

```
APP/APP（自研 ~45,000 行，精读 100%）
├── Core（3,080）      启动链/4 任务/事件总线/模块表     [04]
├── SystemServices（25,290） 全部 25 模块               [05-15]
│    HOSTLINK 协议栈 / 变量 / 日志 / 看门狗 / 崩溃管理 / Shell / 存储 / OTA 代理 /
│    LA / 信号发生器 / 音频 / IMU / 摄像头 / 触摸 / 内存池
├── Application（10,502） 全部模块                       [16-26]
│    网络服务（eth/tcp/icmp/dns/sntp/mqtt/http）/ OTA 四通道 / GUI / 交互反馈 /
│    控制算法库（PID14+KF15+滤波14）/ 数据代理
└── BSP（6,012）       下一阶段 [27+]
```

**APP 三大架构特征**（贯穿 23 篇）：
1. **事件驱动 + 模块化**：模块间零直接调用，全走事件总线（CCM 静态池）
2. **三通道统一命令框架**：UART/TCP/CAN 共用 52 条命令 + 输出路由
3. **四通道 OTA**：UART/TCP/HTTP/CAN 共用 ota_agent 下载核心

## 四、待读清单（下一课——BSP 层）

- [x] cmd_can / test_ctrl（本轮完成）
- [ ] **BSP 层（16 模块，6,012 行）**：lcd(1195+1568) / w25q128(526) / lcd 驱动(444) / can(313) / eeprom(281) / es8388(246) / touch(242) / sram(226) / mpu6050(187) / i2s(161) / uart(158) / power / system / watchdog / buzzer / gpio / flash / rtc
- [ ] 之后转 HOST 上位机（9.7k 行）

## 五、自测题

1. CAN Shell 的行帧组 seq/last 怎么工作？乱序为什么整组丢弃？
2. CAN Shell 与 CAN OTA 的帧 ID 分别是什么？怎么防串扰？
3. test_ctrl 的"等价性测试"验证什么？（不同 KF 实现结果一致）
4. 陷波器测试怎么验证 50Hz 衰减？（注入正弦测幅值）
5. 模糊 PID 的全格扫描验证什么？（输出界）
6. 算法库测试在哪跑？（主机 CI auto_hosttest）
7. CAN Shell 输出切帧的 seq 怎么递增？末帧标志怎么置？
8. 行帧超长怎么处理？（丢弃重来）
