# 会话交接快照（2026-08-15 会话）

> 本文件供新会话/新 Agent 快速接续。会话内所有工作已提交推送
> （HEAD = db2ebf8）。权威约定见 AGENTS.md，细节复盘见各 ENGINEERING_LOG。

## 一、本会话完成的工作（按时间线）

1. **项目理解 + 三子代理深度架构评估**（APP/BOOT/HOST），评级 B+，输出问题清单
2. **全面修复**（4 个提交：932f8c7 workflow / 4ff7f91 boot / bea7fbc app / a8b83a7 host）：
   - CI 结构错误、VLink 分片 bug、文档漂移方案A→B、版本单源化
   - BOOT halt→自愈、APP 并发互斥/超时自愈、mem_map.h
   - PS5.1 GBK 误读根因（workflow ps1 统一 UTF-8 BOM）
   - 主机单测补充（ymodem FSM / OTA_Tool 打包契约 / VLink 分片）
3. **LVGL 性能拉满 + 花屏根治**（58ba24d / 22e5152）：
   - 绘制缓冲外SRAM→主SRAM、REFR 30→16ms、gui bench 基准命令
   - ST7789 窗口写错位根治（逐行单行窗口，selftest 证据链）
   - 多层防花屏守护（上电自检/运行时抽检/编译期断言/verify 守门）
4. **DMA 调试两轮反转**（ee3b235 / db2ebf8）：
   - 10.56 误判"M2M 不可用"→ 10.57 更正：**M2M 完全可用**
   - 正确配置：DIR_1（10=M2M 两比特编码）+ PAR=源 + M0AR=目的 + PINC
   - 工程结论：DMA 直写可用但逐行窗口下无收益（10 vs 18 MPix/s），CPU 写为最优
   - AGENTS.md 新增调试红线（DAP 优先、禁打印-烧录循环）

## 二、当前硬件/固件状态

- 板上固件：v213 build 9192（CPU flush + 防花屏守护 + selftest 全绿）
- 烧录方式：DAP 救援烧录（OTA 通道在 9161 时代曾死，现在板子运行正常但
  **OTA 通道状态需重新验证**——TCP :9020 / ping 192.168.10.10 应重新确认）
- 参数区 last_build_no=9161（DAP 烧录不更新；下次 OTA 需 build > 9161）
- 遗留疑点：参数区 last_error 曾现 0x080E0004（CRC 验证合法值应为 0），未根治

## 三、关键教训（新会话必读，防重踩）

1. **ST7789 窗口写**：单点窗口只收 1 像素；多行窗口 8 方向全 FAIL——**唯一正确
   路径 = 逐行单行窗口**（BSP_LCD_WritePixels/lcd_fill 已是正确实现，勿改回连续写）
2. **F407 DMA**：DIR 是 2 比特编码（00=P2M/01=M2P/10=M2M）；M2M 时 PAR=源、
   M0AR=目的；CCM 不可 DMA（任务栈在 CCM，DMA 缓冲必须主 SRAM）
3. **DAP 取证陷阱**：单命令会话断连会复位目标；halt 下 DMA 不运行——必须
   debug 单会话内 resume/sleep/halt；流基址查 CMSIS 宏（流间隔 0x18）
4. **PS5.1**：workflow ps1 必须 UTF-8 带 BOM；edit 类工具写文件会丢 BOM
5. **UV4 构建**：auto_build 的日志可能陈旧误报 OK——直接 Start-Process UV4
   并检查日志时间戳；构建失败时**先纠错再烧录**（编译→0 错误门→烧录）

## 四、下一步建议（顶级 GUI 打造）

1. 基础层已拉满（CPU flush 18MPix/s 是面板带宽上限）：全屏填充 59fps、
   局部动画 318fps、骨架 UI 17fps
2. **UI 帧率瓶颈在渲染侧**（对象遍历+外部SRAM堆读+圆角 layer 混合，
   占帧时间 90%）——界面打造方向：对象精简合并、圆角/阴影按需、
   静态区 canvas 缓存、避免全屏 invalidate
3. 先验证 OTA 通道恢复（ping + TCP 9020），恢复"APP 走 OTA"约定
4. 建议下一会话从 `gui bench` / `lcd selftest` 复测基线开始
