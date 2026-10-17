# 01_bringup 工程日志（重点问题复盘）

> 记录格式遵循 `D:\GIT-SPACE\D00\docs\ISSUE_POSTMORTEM_TEMPLATE.md`。
> 收录标准：需要多步排查、过程中出现过被推翻的方向、根因属系统性（并发/时序/底层配置）、
> 或最终采用工程权衡的问题。**分析路径比结论更重要**，所以每条都保留完整排查过程。

## 索引

| # | 标题 | 类型 | 状态 |
| --- | --- | --- | --- |
| 1 | LCD 只有第一个汉字「董」正常，后两字显示为背景白 | 并发 / 驱动使用方式 | 已修复，实机复测通过 |
| 2 | `.\idf.ps1` 必报 `idf.py : 无法将"idf.py"项识别为 cmdlet` | 工具链 / 编码 | 已修复，实机复测通过 |
| 3 | 给 `main` 组件写 `REQUIRES` 后编译报 `esp_flash.h: No such file or directory` | 构建系统 / 组件依赖 | 已修复 |
| 4 | 音频通路存在两版硬件：ES8311 必须先有时钟再配置 | 时序 / 硬件版本差异 | 已按"自动识别 + 先起 I2S"处理 |
| 5 | TF 卡插着却报"无卡"（`ESP_ERR_NOT_FOUND`），格式化/重挂载都无效 | 时序 / SD 规范上电时钟 | 已修复（挂载前补 80 时钟） |

---

## 1. LCD 只显示第一个汉字「董」正常，后两字显示为背景白

### 现象

- 开机名字卡功能首次上机：屏幕上「董」笔画清晰完整，**「衍」「俊」两个字的位置是纯白（即背景色）**，
  看起来像被擦掉了。
- 自检日志本身无异常：`LCD: init OK (320x240, ST7789 8bit-I80, backlight on)`，
  `name_card_draw()` 正常走到结束并打印 `drawn "董衍俊": 3 glyphs 64x64, area x=54..265 y=88..151`，
  240 行 `lcd_draw_bitmap()` 全部返回 `ESP_OK`（没有任何 `ESP_ERR_*` 被 `main` 记录）。
- 关键几何信息：三个字分别占 x=54..117 / 128..191 / 202..265，行范围 y=88..151，
  **三个字在完全相同的行上**，所以"按行丢失"解释不了，只能是"同一行内 x≥128 的部分没有上屏"。

### 影响面

整块 LCD 的显示功能。逐行刷屏（`lcd_fill()` / 任何"每行复用同一块行缓冲"的画法）都会中招；
只是整屏单色填充看不出错乱，所以不容易早期发现。

### 排查思路（动手前的方法论）

这个现象天然分成两条腿，先想办法把其中一条彻底切断，再集中打另一条：

1. **数据腿**：字模数据是不是空的/错位的（C 源码 → 目标文件 → 运行时指针）。
2. **传输腿**：数据对，但没送到屏幕上（驱动限长、配置抄错、时序/竞态）。

切分手段：①把生成的 C 数组**反向解析回位图**看墨迹量；②查 ELF 里的符号地址与内容；
③读 IDF 驱动源码，确认"一次传输到底能送多少字节"；④与厂商例程逐项对照排除配置差异。

### 排查过程

**第 1 步：怀疑字模数据是空的（被推翻）**

做法：写了 `tools/check_font.py`，把 `main/app/font_name_64.c` 里的三个数组成员按位图解析回来，
统计非零字节数、墨迹像素数与有效行范围。

观察：

| 字模 | 声明长度 | 解析长度 | 非零字节 | 墨迹像素 | 有效行范围 |
| --- | --- | --- | --- | --- | --- |
| `s_alpha_8463`（董） | 4096 | 4096 | 1813 | 1576 | 3..59 |
| `s_alpha_884D`（衍） | 4096 | 4096 | 1120 | 847 | 2..60 |
| `s_alpha_4FCA`（俊） | 4096 | 4096 | 1614 | 1301 | 2..61 |

结论：三个字都有大量墨迹，数据完整。**"字模是空的"这个方向排除**。

**第 2 步：怀疑运行时指针/数据不对（被推翻）**

做法：用 `xtensa-esp32s3-elf-nm -S` 看实际参与编译的 `build/box_bringup.elf`，
再用 `objdump -s` 把符号内容 dump 出来。

观察：

```
3c03b6c4 00000018 D name_glyphs        # 3 项 × 8 字节
3c03b6dc 00001000 d s_alpha_4FCA       # 各 4096 字节
3c03c6dc 00001000 d s_alpha_884D
3c03d6dc 00001000 d s_alpha_8463

3c03b6c4 63840000 dcd6033c 4d880000 dcc6033c  |  0x8463,0x3c03d6dc  0x884D,0x3c03c6dc
3c03b6d4 ca4f0000 dcb6033c 00000000 00000000  |  0x4FCA,0x3c03b6dc
```

映射表里三个指针与 `nm` 报出的数组地址**一一吻合**；再 dump 有笔画的行（每字 row4~row7），
三块都有真实灰度值（例如 `884D` 的行 4 出现 `0x13e9 / 0xffffc41e / 0x3a / 0x59`）。

结论：**"指针错/数据被优化掉/链接丢段"这条腿也被切断**。

**第 3 步：怀疑驱动把每行传输截断（症状很像，但被推翻）**

为什么怀疑它：面板开了 `invert_color`，GRAM 未被写入的区域（0x0000 反色后）**正好显示为白**。
于是"每行只送了前 ~256 字节（=128 像素）"能完美解释观察到的现象——「董」落在 54..117 之内
完好无损，「衍」从 128 开始正好整段落在外面。

做法：读 IDF 源码 `components/esp_lcd/src/esp_lcd_panel_io_i80.c` 与 `esp_lcd_common.c`。

观察：

```c
// esp_lcd_panel_io_i80.c:135
size_t num_dma_nodes = bus_config->max_transfer_bytes / DMA_DESCRIPTOR_BUFFER_MAX_SIZE + 1;
// esp_lcd_panel_io_i80.c:472
assert(color_size <= (bus->num_dma_nodes * DMA_DESCRIPTOR_BUFFER_MAX_SIZE)
       && "color bytes too long, enlarge max_transfer_bytes");
// hal/dma_types.h
#define DMA_DESCRIPTOR_BUFFER_MAX_SIZE (4095)
```

代入本工程参数：`max_transfer_bytes = 320*240*2 = 153600` → `num_dma_nodes = 38`，
可容纳 38×4095 = 155610 字节；一行只有 640 字节，**断言根本不会触发**，
`lcd_com_mount_dma_data()` 也是按 4095 分片的。结论：**驱动层不存在 256 字节截断**，
这个方向排除（虽然症状高度吻合，是本次最容易误判的岔路）。

**第 4 步：怀疑配置抄错（被推翻）**

做法：把厂商 v5.2.1 例程（`basic_routines/21_chinese_display`）和 v5.4 例程
（`21_chinese_display` / `10_lcd`）的 `lcd.c` 拉出来逐项对照：总线参数、`pclk_hz`、
`trans_queue_depth`、`max_transfer_bytes`、`dc_levels`、`swap_color_bytes`、面板注册参数、
`reset/init/invert_color/set_gap/MADCTL 0x36=0x00/COLMOD 0x3A=0x65/swap_xy/mirror/disp_on_off`。

观察：两版例程**完全一致**，与本工程实现也逐项等价。结论：**不是配置抄错**，排除。

**第 5 步：回到"提交即返回 + 零拷贝"这条线（命中）**

做法：把 `panel_io_i80_tx_color()` 完整读一遍。

观察（关键三行）：

```c
// esp_lcd_panel_io_i80.c:479-508
if (i80_device->num_trans_inflight < i80_device->queue_size) {
    trans_desc = &i80_device->trans_pool[i80_device->num_trans_inflight];
} else {
    xQueueReceive(i80_device->done_queue, &trans_desc, portMAX_DELAY);  // 池满才阻塞
    ...
}
trans_desc->data = color;                    // 零拷贝：直接指向调用方缓冲区
xQueueSend(i80_device->trans_queue, &trans_desc, portMAX_DELAY);
i80_device->num_trans_inflight++;
esp_intr_enable(bus->intr);
return ESP_OK;                               // 立刻返回，DMA 在 ISR 里才真正开跑
```

结论：`esp_lcd_panel_draw_bitmap()` 返回时，**那块缓冲区仍然归 DMA 所有**，
最多可积压 `trans_queue_depth`（本工程 =10）笔未完成的传输。
而当时的实现是逐行刷屏、240 行**复用同一块 640 字节行缓冲**——
每提交一行就立刻重写它，直接违反了"传输完成前缓冲区内容必须保持不变"这个契约。
**根因命中。**

### 根因

**把"异步零拷贝"的接口当成同步接口用了：一次提交之后立刻复用了同一块缓冲区，
DMA 读到的是被改写过、甚至只写了一半的内容。**

机理细说：

- 一行 640 字节，在 `pclk_hz = 10MHz`、8bit 并口下要**约 64µs** 才搬完；
  而 CPU 重写一行只要几微秒。传输一开始，缓冲区就已经在变。
- 名字卡渲染一行时的写入顺序是：**先整行铺底色 → 再依次写「董」→「衍」→「俊」**。
  于是缓冲区会短暂处于"底色已经铺好、后面两个字还没写"的中间态。
  若 DMA 在这个瞬间取数，送到屏幕的就是「白底 + 董 + 空白」——
  与"后两个字被擦成白色"的现象完全吻合。
- 取数的具体瞬间由中断/调度时序决定，所以它表现为一个**稳定的怪现象**而不是随机花屏；
  这也正是前三步用"数据 / 传输长度 / 配置"都解释不通的原因。

### 解决方案

| 文件 | 改动 |
| --- | --- |
| `main/app/name_card.c` | 改为**整帧渲染**：在 PSRAM 申请 320×240×2=150KB、64 字节对齐的帧缓冲（`heap_caps_aligned_alloc`，对齐值对应总线的 `psram_trans_align=64`），先把整帧画完，再用**一次** `lcd_draw_bitmap(0,0,320,240,fb)` 整屏推送；之后等传输完成。帧缓冲常驻不释放，避免"刚提交就把缓冲区还回堆"。 |
| `main/bsp/lcd.c` / `lcd.h` | 新增 `lcd_color_trans_done_cb()`（中断回调 `xSemaphoreGiveFromISR`）与 `lcd_wait_flush_done(timeout)`；`lcd_fill()` 改为**逐行等完成**再改行缓冲；`lcd_draw_bitmap()` 提交前先清掉残留完成信号，保证等到的是"这一次"传输。 |

为什么这样改有效：整帧方案下**一块数据只有一笔传输，且传输期间内容不再被改写**，
从结构上消除了"边传边改"；`lcd_wait_flush_done()` 则把"传输结束"这个事实显式暴露出来，
使缓冲区可以安全复用/释放。

> 附带产物：`NAME_CARD_DIAG_FRAME`（默认 1）会在屏幕四边画 1 像素自检边框，
> 用来一眼确认"整屏每个像素都真的写进去了"；不需要时改为 0 重新编译即可。

### 验证

- 实机复测（2026-10-13）：重新编译烧录后，「董」「衍」「俊」三个字**完整正确显示**，
  用户确认"完美，可以了"。
- 日志侧：`LCD: init OK`、`NAMECARD: rendered "董衍俊": 3 glyphs 64x64, area x=54..265 y=88..151 (frame=1)`，
  无任何 `LCD` / `NAMECARD` 错误。
- 回归面：`lcd_fill()` 走的是同一条"等完成再复用"的路径。

### 经验沉淀

1. **i80 / SPI 面板 IO 的 `tx_color` 都是"零拷贝 + 异步"的**：`draw_bitmap()` 返回 ≠ 传完。
   要么每笔传输用各自独立的数据块，要么用 `on_color_trans_done` 等完成后才能改/放。
   这条契约在 IDF 源码里写得很清楚，但头文件注释没有强调，容易被"提交即完成"的直觉骗过。
2. **排除法要先把"数据"这条腿彻底切断**：`tools/check_font.py`（C 源码→位图回读）、
   `nm -S` + `objdump -s`（ELF 符号与内容）两步就能把数据类怀疑全部消掉，
   剩下的范围会小很多。这两个手段已固化到本工程，可复用。
3. **反色面板 + 未初始化 GRAM=0 会显示成白**：很容易把"没写进去"误判成"写成了白色"。
   遇到"某块区域是纯背景色"时，先想清楚"是渲染成了背景"还是"根本没送到"。
4. **厂商例程同样有这个隐患**：`lcd_clear()` 也是逐行复用同一缓冲，`lcd_draw_point()`
   甚至把栈上变量的地址直接交给 DMA。它们没暴露，是因为整屏同色填充看不出错乱、
   单点 2 字节在 200ns 内就读完了。**只要"每行内容不同"，这个坑必然现形。**
5. 排查过程中出现过的错误方向（本次最值钱的部分）：
   - ✗ 字模数据为空 → 实测墨迹 1576/847/1301，排除；
   - ✗ 运行时指针/链接丢段 → ELF 里三个指针与数组地址一一对应，排除；
   - ✗ 驱动把每行传输截断到 256 字节 → 源码确认 38 个描述符可容纳 155610 字节，排除；
   - ✗ 配置抄错 → 与厂商两版例程逐项一致，排除。

---

## 2. `.\idf.ps1` 必报 `idf.py : 无法将"idf.py"项识别为 cmdlet`

### 现象

`.\idf.ps1 build` 必然失败并提示 `idf.py` 不是可识别命令；同时 `export.ps1` **一句输出都没有**
（正常情况下会先打印 `Setting IDF_PATH` / `Adding ESP-IDF tools to PATH...` / `Done!`）。

### 排查过程

1. 先怀疑 `. export.ps1 | Out-Null`：管道会给脚本另开作用域，函数定义传不回来 → 去掉管道重试。
   **结果：现象不变，方向推翻**（但"不要加管道"这条结论仍然成立，已在脚本里写明原因）。
2. 用 `powershell -Command` 手工 dot-source 同样的 `export.ps1`：**完全正常**，
   `idf.py --version` 能打印 `ESP-IDF v5.2.1`。→ 说明 IDF 本身没问题，问题出在 `idf.ps1` 这个文件。
3. 检查文件属性：`bytes=1791，无 BOM，LF 换行`；再让 PowerShell 解析器 `Parser::ParseFile` 检查语法：
   **无语法错误**。
4. 对比两次实验的唯一差异：`idf.ps1` 里含**中文注释**，而它是**无 BOM 的 UTF-8**。
   Windows PowerShell 5.1 对无 BOM 脚本按 ANSI(GBK) 解码，多字节歧义会把
   **行尾换行一起吃掉**，于是 `. 'E:\...\export.ps1'` 这一行被并进上一条注释、从未执行 ——
   `export.ps1` 没跑，`idf.py` 函数自然不存在，也就解释了"零输出 + 命令找不到"。

### 根因

无 BOM 的 UTF-8 ＋ 中文注释，在 GBK 默认代码页的 Windows PowerShell 5.1 下被错误解码，
**把下一行代码吞成注释**。属于文件编码级问题，与控制台显示乱码是同一类根因。

### 解决方案

把 `idf.ps1` 的注释**全部改成纯 ASCII**，并在文件头写明"本文件必须保持 ASCII"的原因，
与 `sdkconfig.defaults` 的纪律一致。顺带保留"不要给 dot-source 加管道"的说明。

### 验证

`.\idf.ps1 --version` → `ESP-IDF v5.2.1-dirty`；`.\idf.ps1 build` 正常走完。

### 经验沉淀

仓库里所有**会被 Windows 工具直接解析的脚本/配置文件**（`.ps1`、`sdkconfig.defaults`、`Kconfig`）
一律保持纯 ASCII；需要中文说明就写在 `.md` 文档里。判断依据不是"能不能显示"，
而是"解析器用什么代码页读它"。

---

## 3. 给 `main` 组件写 `REQUIRES` 后 `esp_flash.h` 找不到

### 现象

新增 LCD 代码并把 `main/CMakeLists.txt` 写成
`idf_component_register(SRC_DIRS "." "bsp" "app" ... REQUIRES driver esp_lcd)` 后，
编译到 777/784 时报 `main.c:25:10: fatal error: esp_flash.h: No such file or directory`。

### 排查过程

1. `esp_flash.h` 属于 `spi_flash` 组件，而该组件并没有被删掉，先怀疑是增量构建残留 →
   现象与"新增 REQUIRES"高度相关，优先查构建系统规则。
2. 读 IDF `tools/cmake/project.cmake:614-639`，看到关键注释与判断：

   ```cmake
   # Special treatment for 'main' component for standard projects.
   # This behavior should only be when user did not set REQUIRES/PRIV_REQUIRES manually.
   if((NOT reqs OR reqs STREQUAL managed_reqs) AND (NOT priv_reqs OR ...))
       ... INTERFACE_LINK_LIBRARIES "${build_components}"   # 依赖全部组件
   ```

   即：**只有在 `main` 没有手工声明依赖时**，IDF 才让它自动依赖本次构建的全部组件；
   一旦显式写了 `REQUIRES`，`main` 就只剩列出来的那几个，其余组件的头文件路径全部失效。

### 根因

误把 `main` 当成普通组件显式声明依赖，关掉了 IDF 给 `main` 的"依赖全部组件"这一便利行为
（`project.cmake` 里的 special treatment）。

### 解决方案

`main/CMakeLists.txt` **不写 `REQUIRES` / `PRIV_REQUIRES`**，只用
`SRC_DIRS "." "bsp" "app"` + `INCLUDE_DIRS "." "bsp" "app"` 注册新增目录，
并把上述原因写成注释留在文件里，防止后人再"顺手补上"。

### 验证

改回后 784/784 全量编译通过，产出 `box_bringup.elf` / `box_bringup.bin`。

### 经验沉淀

标准工程的 `main` 组件是"特殊组件"：想省事就用 IDF 的隐式全依赖，想要显式依赖就别指望隐式全依赖，
两者只能选一个。

---

## 4. 音频通路存在两版硬件：ES8311 必须先有时钟再配置

### 现象 / 背景

整理 BSP 时对照资料发现同一个板子存在两个硬件版本的音频通路：

- **V1.0**（《硬件参考手册 V1.0》）：NS4168，纯 I2S 数字功放，**没有 I2C 接口**；
- **V1.1**（`ATK_DNESP32S3B_V1.1.pdf` 原理图）：**ES8311**（I2C 0x18）+ NS4150B 功放；

厂商例程也分两套（`23_music` 与 `23_music(ES8311)`）。
如果 BSP 只按其中一版写死，换一块板就会出现"完全没声音"或"初始化失败"。

### 排查/设计过程

1. 先怀疑是"同一个板上两种芯片都焊了" → 查原理图网表：U6 只有一颗，
   V1.1 是 ES8311、V1.0 是 NS4168，属于**不同批次**，排除共存。
2. 想用"读 WHO_AM_I"区分 → ES8311 没有可靠的 ID 寄存器（0xFD/0xFE 在部分批次不可靠），
   方案不成立。**改用最朴素也最可靠的办法：I2C 探测 0x18 是否 ACK**。
   - 有 ACK → ES8311，走 I2C 初始化；
   - 无 ACK → 按 NS4168（纯 I2S）处理。
   这样两条通路都不需要改代码。
3. ES8311 的 MCLK 来源：板上 **MCLK 引脚悬空（NC）**，而 ES8311 支持用 BCLK 当 MCLK
   （厂商例程正是 `MCLK_SOURCE = FROM_SCLK_PIN`）。
   → 由此推出一个硬约束：**必须在配置 ES8311 之前就让 I2S 出时钟**，
   否则编解码器内部状态机没有时钟、寄存器写完也不工作。
   本 BSP 的初始化顺序因此固定为：建 I2S 通道 → 使能通道（出 BCLK/LRCK）→ 配置编解码 → 开功放。
4. 同一约束还决定了采样率范围：BCLK = 采样率 × 32（16bit 立体声一帧 32bit），
   而 ES8311 的时钟系数表里，能匹配"rate×32"的只有
   **32k / 44.1k / 48k / 64k / 88.2k / 96kHz**。16kHz 录音需要改用 32bit 槽宽，
   这条限制已写进 `audio.h` 与组件 README 的"已知限制"。

### 根因

不是代码 bug，而是**同一型号开发板存在两版音频硬件**，
且 ES8311 的时钟依赖方式（MCLK 取 BCLK）决定了初始化顺序不可颠倒。

### 解决方案

- `bsp_audio_init()` 内置自动识别：`bsp_i2c_probe(0x18)` 命中走 ES8311，
  否则按 NS4168 处理，并把结果通过 `bsp_audio_codec()` 暴露给应用；
- 顺序固定为"I2S 先出时钟 → 再配置编解码 → 最后开功放"（避免上电 POP 声）；
- ES8311 的寄存器序列按厂商 v5.4 例程逐条移植，只把时钟系数表按
  "BCLK 当 MCLK"这一固定用法收敛（pre_div/pre_multi/adc_div/dac_div/osr 固定，
  仅 lrck_l 与 bclk_div 随采样率变化）；
- 不支持的采样率在 `bsp_audio_configure()` 与 `esp8311_init()` 里都显式报错，
  而不是"静默出噪声"。

### 验证

- 编译期：`bsp_audio_configure()` 拒绝非 16bit/立体声配置；不支持采样率在 init 时报
  `ESP_ERR_INVALID_ARG` 并给出支持列表；
- 运行期：`bsp_print_info()` / `bsp_selftest(true)` 会打印
  `selftest audio @0x18 : ES8311 present` 或 `NS4168 (pure I2S)`，
  一眼看出当前板子是哪个版本。

### 经验沉淀

1. **同一型号开发板可能有多个硬件版本**：动手写驱动前，把"原理图版本号 + 同系列不同版本例程"
   都对照一遍，比事后调试"为什么没声音"便宜得多。
2. 外置编解码的**时钟依赖**决定初始化顺序：MCLK 来自主控时先配编解码再开 I2S；
   MCLK 来自 BCLK/SCLK 时必须先让 I2S 出时钟。这条对 ES8311/ES8388 之类都适用。
3. 能"探测得出"的差异就不要写成编译开关：I2C 探测成本几乎为零，
   却省掉了用户改配置、重新编译的一整轮。

---

## 5. TF 卡插着却报"无卡"：SD 规范要求的"上电 74 时钟"没人补

### 现象

- 插着 32GB TF 卡开机，屏幕自检「卡」项显示 **`无卡/未插好`**，
  驱动日志为 `mount failed: ESP_ERR_NOT_FOUND`（0x105）。
- 长按 K2（重新挂载）无效；长按 K1（格式化并挂载）也无效。
- 存储行一直停在"无卡/未插好"。

### 影响面

TF 卡完全不可用（挂载、容量查询、文件读写全部不可用）。

### 排查思路

这个故障天然有两条腿，先想办法一刀切开，再分别处理：

1. **文件系统层**：卡能应答、但格式不对（exFAT 之类）→ 表现应是 `ESP_FAIL(0x101)`；
2. **电气层**：卡根本没应答 → 表现应是 `ESP_ERR_NOT_FOUND(0x105)` / `ESP_ERR_TIMEOUT(0x107)`。

切分手段：**绕过整个 SDSPI/FATFS 协议栈，用普通 GPIO 手敲最小时序（74 时钟 + CMD0 + CMD8）**，
看卡到底回不回答。这是"硬件取证"，能直接判定是卡/卡座的问题还是代码/配置的问题。

### 排查过程

**第 1 步：怀疑格式（被降级为次要因素）**

查 `components/fatfs/src/ffconf.h`：`FF_FS_EXFAT 0` —— IDF 5.2 的 FATFS **编译期就没开 exFAT**，
所以 exFAT 卡一定挂不上。但本例的错误码是 `NOT_FOUND` 而不是 `FAIL`：
FATFS 是在"卡已被发现、读分区表"阶段才会失败，而 `NOT_FOUND` 说明卡连"被发现"这一步都没过。
结论：格式可能是后续障碍，但不是当前这个报错的原因。

**第 2 步：怀疑配置抄错（被推翻）**

逐项对比厂商 `19_sd` 例程：SPI host 选择（SPI2）、`SDMMC_HOST_FLAG_SPI`、
`max_freq_khz`、slot 配置（CS=IO17）、引脚（SCK=IO7 / MISO=IO15 / MOSI=IO16）、
以及"先 `spi_bus_initialize` 再 `esp_vfs_fat_sdspi_mount`"的流程 —— 全部一致。
我唯一的差异是 `max_transfer_sz`（4096 vs 153600）与 `allocation_unit_size`（16K vs 4K），
两者都不参与"发现卡"这一步。结论：**不是配置抄错**。

**第 3 步：读驱动源码定位错误码来源（命中关键线索）**

`components/driver/spi/sdspi/sdspi_host.c:654`：

```c
if (++ncr_cnt > 8) return ESP_ERR_NOT_FOUND;
```

即：发完一条命令后，驱动会连续读 8 个字节等卡把 MISO 拉低（NCR 等待窗口）；
一个都没等到就判"没有卡"。这是**电气层无应答**的典型证据。

**第 4 步：加"裸 SPI 探测"做硬件取证（决定性实验）**

新增 `bsp_tfcard_probe_raw()`：把 SPI 引脚临时收回为普通 GPIO，手动实现 SPI 模式 0 的
位敲（约 400kHz），依次发：74 个时钟（CS 高）→ CMD0 读 R1 → CMD8 读 R7 回显。
结果直接显示到屏幕的「SD探测」行，不需要连串口。

**用户实测结果出乎意料**：探测跑过之后，卡就活了 —— 随后挂载成功，
存储行显示 `29810M free`（32GB 卡）。

这个反常现象本身就把根因指出来了：**探测比 IDF 的挂载流程多做了一件事 ——
它在命令之前先发了 74 个时钟。**

**被推翻/修正的方向汇总**：

- ✗ 卡座接触不良（让用户反复插拔、换卡）—— 解释不了"探测之后就能挂上"；
- ✗ exFAT 格式（让用户格式化）—— 报错码是 NOT_FOUND 不是 FAIL，卡根本没被认到；
- ✓ 真正原因是**上电时钟没补足**。

### 根因

**卡没能进入 SPI 模式，所以对所有命令都不应答。**

SD 卡上电后处于原生（SD）模式，要切到 SPI 模式必须满足两个条件缺一不可：
1. 上电后先给 **≥74 个时钟**且 **CS 保持高**（Part 1, 6.4.1.1）；
2. 随后发一条 **CS 拉低的 CMD0**，卡才会真正切到 SPI 模式并开始应答。

把 IDF 的 SDSPI 源码翻到底（`sdspi_host.c`）：`configure_spi_dev(SDMMC_FREQ_PROBING)`
确实是 400kHz + mode 0，CMD0 前也确实调了 `go_idle_clockout()` 发 80 时钟，
init_device 里也 `cs_high()` —— 该做的都做了，**但在这块板 + 这张卡上就是不生效**。

决定性证据来自"分步实验"：

| 挂载前做了什么 | 结果 |
| --- | --- |
| 什么都不做（IDF 原流程） | ✗ `ESP_ERR_NOT_FOUND` |
| 只补 80 个时钟（CS 高，不发 CMD0） | ✗ 仍然 `ESP_ERR_NOT_FOUND` |
| 手敲 74 时钟 + **CS 拉低的 CMD0** + CMD8 | ✓ 卡应答 R1=0x01、CMD8 回显 0x1AA，紧接着挂载成功 |

所以"进 SPI 模式"这一步实际卡在 CMD0 上：**只有我们自己手敲的那条 CMD0 起了作用**。
（IDF 内部那条 CMD0 为什么在这张卡上不生效，受限于手头没有逻辑分析仪没能进一步定位；
但结论可复现、修复有效。）

### 解决方案

在 `bsp_tfcard_init()` 里，**挂载之前**固定执行一次"手敲进 SPI 模式"：
直接复用裸探测 `bsp_tfcard_probe_raw()`（它本来就是 74 时钟 + CMD0 + CMD8 的完整时序），
内部流程是：

1. 临时把 SPI 引脚从外设手里收回（`bsp_spi_bus_deinit()` + 配成普通 GPIO）；
2. CS 高发 80 个时钟 → CS 拉低发 CMD0 读 R1 → 发 CMD8 读 R7 → CS 拉高；
3. 把引脚还给 SPI 总线（`bsp_spi_bus_init()`），随后正常交给 SDSPI 挂载。

代价约 2~3ms，只在挂载时发生一次；卡若没应答也会打日志提示（R1 值一并打印）。
好处是"诊断"和"修复"用同一条代码路径 —— 屏幕上「SD探测」行显示的，
就是挂载前真正执行的那套时序。

### 验证

- 用户实测（修复前）：手动探测之后立刻挂载 → 成功，存储行显示 `29810M free`
  （32GB 卡，已格式化为 FAT32）；
- 用户实测（只补 80 时钟的中间版本）：仍报 `无卡/未插好`，同时「SD探测」正常
  —— 这条对照实验正是定位到"CMD0 才是关键"的依据；
- 待复测：本版（挂载前固定手敲 CMD0）断电冷启动应一次挂上。

### 经验沉淀

1. **先看错误码再动手**：`NOT_FOUND/TIMEOUT` = 电气层没应答（查线/查卡座/查时序）；
   `ESP_FAIL` = 卡已认到但文件系统不对（查格式）。方向完全不同，别混。
2. **SD/SPI 类器件"起不来"时，用普通 GPIO 手敲最小时序做硬件取证**，
   比反复怀疑代码、反复换卡快得多 —— 本例一眼就看出"探测能让卡活"这个关键线索。
3. **规范的硬性时序要求，自己补一遍最保险**：74 时钟成本 1ms，却能避免一整类
   "偶发/依赖具体卡"的挂载失败。

---

## 6. LVGL 移植与显示性能调优：从 37fps 到 47fps 的根因定位

### 现象（目标）

要求"LVGL 移植 + 界面丝滑"。硬件是 320x240 ST7789、**8080 并口 8bit @10MHz**，
即总线带宽 10MB/s，全屏 150KB 理论 15.4ms ≈ **65fps 上限**。
首版跑通后实测全屏重绘 26ms/帧（38fps），距上限还有 10.6ms 不知去向。

### 影响面

页面切换动画（MOVE_LEFT 每帧都是全屏重绘）与整屏滚动会明显掉帧；
静态页面无感，动态体验不达标。

### 排查思路

把"每帧 26ms"拆成两段：**总线搬运**（已知 15.4ms，可算）与**软件渲染**（未知）。
只可能从这两处下手，所以先做一个可复现的量化自测，再逐项排除。

### 排查过程（三轮实测，数据驱动）

自测方法：上电 3s 后跑一次 lv_obj_invalidate(全屏) + lv_refr_now() × 20 次，
用 esp_timer 计时取平均（结果打印为 PERF full-screen redraw）。

| 轮次 | 改动 | 全屏重绘 | 结论 |
| --- | --- | --- | --- |
| ① 基线 | 缓冲 320x40 x2（内部 RAM），双缓冲 + 异步 DMA | **26 ms → 38 fps** | 渲染耗时 ≈ 10.6ms，占主导 |
| ② 加大缓冲 | 320x40 → 320x60（每帧 6 块 → 3 块） | **27 ms → 37 fps** | **无改善 → 排除"分块开销"假设** |
| ③ 热路径入 IRAM | LV_ATTRIBUTE_FAST_MEM = IRAM_ATTR | **21 ms → 47 fps** | **确认根因：flash XIP 取指 cache miss** |

（③ 的代价：内部 RAM 少 ~15KB，仍余 172KB；flash +0.6KB。）

被推翻的方向：**"分块太多导致每次 flush 有固定开销"**——把每帧分块数减半后
帧率不升反降 1ms，说明瓶颈不在提交/中断次数，而在每像素的 CPU 时间。

### 根因

LVGL 的混合/字形/填充函数默认放在 flash 里执行（XIP）。8080 并口搬运期间
CPU 正在渲染下一块，此时大量指令 cache miss 会把渲染时间推到 10ms 量级，
把本该 15.4ms 的帧硬生生拖到 26ms。LVGL 官方为此预留了 LV_ATTRIBUTE_FAST_MEM
标注，但默认定义为空；ESP32 上把它映射成 IRAM_ATTR 即可让热点代码常驻 IRAM。

### 解决方案

`c
/* components/lvgl/lv_conf.h */
#include <esp_attr.h>
#define LV_ATTRIBUTE_FAST_MEM  IRAM_ATTR
`

刷新通路（components/ui/src/ui_port.c）同时固定为：

1. **双缓冲放内部 RAM**（2 × 320×60 × 2B = 76.8KB），PSRAM 写带宽不足；
2. **异步提交 + 完成回调**：sp_lcd_draw_bitmap_async() 提交后立即返回，
   DMA 完成中断里 lv_disp_flush_ready() —— 渲染下一块与搬运本块重叠；
3. **单笔在途约束**：flush_cb 内若上一笔未完成则等 BSP 完成信号量（保证不踩缓冲）；
4. **LVGL 任务独占 core1**（prio 5，12KB 栈），应用逻辑留 core0；
5. 时基用 LV_TICK_CUSTOM + esp_timer_get_time()，省掉 1ms 定时器中断。

### 验证

`
I (2983) UI: LVGL 8.3.5 ready: 320x240, buf=320x60 x2 (internal RAM), core1
I (6357) UI: PERF full-screen redraw: 21 ms/frame -> 47 fps (bus limit ~65 fps @10MHz)
I (6358) UI: PERF mem: internal heap free=172679 B | LVGL heap 26% used (16424/65536 B)
I (7923) BRINGUP: [BEAT ] #2 uptime=7 s | heap free=8364496 B | UI fps=22
`

- 全屏重绘 47fps、局部刷新（时间/温度/光照每 250ms）几乎零延迟；
- APP 体积 739,984 B + LVGL 后 743,200 B，分区余量 63%（1.29MB）;
- 上电流程、三页仪表盘、触摸点击页签、K1/K2 切页均正常。

### 经验沉淀

1. **先量化再优化**：把"慢"拆成可算的（总线）与不可算的（渲染），
   三步就能锁定 IRAM 这个根因；否则容易在"双缓冲/分块策略"上反复折腾。
2. **一次只改一个变量并记录数字**：②轮"加大缓冲"是反直觉的失败实验，
   但正是它排除了错误方向——失败实验同样要进日志。
3. **ESP32 上凡是"CPU 密集 + flash 常驻"的库，都要考虑 IRAM 化**：
   LVGL 有现成开关，mbedtls/音频算法同理（代价是内部 RAM）。
4. **8080 并口屏的帧率天花板由总线算得出**：16bit 像素 × 8bit 并口 = 2 拍/像素，
   10MHz 下 65fps；想再高只能提 pclk（15MHz → 约 98fps 理论上限，
   但 ST7789 写周期下限 66ns，且需示波器确认信号完整性）。

---

## 7. 显示性能第二轮：21ms → 17ms，并把瓶颈彻底锁定在"渲染"

### 背景

第一轮把全屏重绘从 26ms 优化到 21ms（IRAM 热路径）。继续追问"是否已到极致"时，
发现之前的归因方法（用"总时间 - 总线理论时间"反推）是错的——它会随着总线变快而
把误差算到渲染头上。于是改成**直接测量**。

### 方法：四条独立基准（上电 3s 自动跑一次，UI_STARTUP_BENCH 可关）

| 基准 | 测什么 | 15MHz 实测 |
| --- | --- | --- |
| ① bus-only | 整屏 150KB 一次 DMA（绕过 LVGL） | **10 ms → 100 fps（硬天花板）** |
| ② lvgl full redraw | 我们的仪表盘整屏重绘 | **17 ms → 58 fps** |
| ③ breakdown | flush_cb 内部耗时 / 两次 flush 之间的渲染耗时 | **flush_cb 0 ms + render 16 ms** |
| ④ plain screen | 纯色背景 + 一个标签（框架地板） | **10 ms → 100 fps** |

### 结论（三条硬事实）

1. **框架层零开销、已完全重叠**：极简页面 10ms = 总线 10ms，
   说明双缓冲 + 异步提交 + 完成回调这条通路的流水线是**最优的**，
   lush_cb 实测 0ms（提交不阻塞、无等待）。
2. **瓶颈是渲染（16ms）**，不是总线，也不是框架。
3. 由此**排除了**此前设想的"再加一路 DMA 暂存池做三缓冲流水"——
   在 flush_cb 已 0ms 的前提下，那是无用功（这条假设是被数据推翻的）。

### 过程中抓到的两个真实 WAS

| # | 现象 | 根因 | 修复 | 收益 |
| --- | --- | --- | --- | --- |
| ① | 10MHz→15MHz 后总线 15ms→9ms，但整帧只从 21ms 降到 19ms | 总线变快后渲染占比上升，之前"总时间-总线"的归因失真 | 改为直接测量 flush_cb 与渲染段 | 拿到真实拆分 |
| ② | 拆账显示渲染 19ms，但单测大字/圆弧都"+0ms" | **lv_led 默认用 shadow 模糊伪造发光**，逐像素模糊极贵；单测之所以看不出来，是因为那些测试被总线卡在 10ms（渲染未超窗，测不出差值） | lv_obj_set_style_shadow_width(led, 0, LV_PART_MAIN) | **20→17ms（-3ms）** |

补充踩坑：性能基准里**先 lv_scr_load(临时屏) 再 lv_obj_del(临时屏)** 会当场崩溃重启——
必须先切回原屏再删。已修正并在代码里注明。

### 当前账本（15MHz / 8bit i80 / 320x240）

`
总线天花板  10.0 ms/帧  (100 fps)  ← 8080 物理极限
框架地板    10.0 ms/帧  (100 fps)  ← 极简页面 = 总线，零额外开销
本仪表盘    17.0 ms/帧  ( 58 fps)  ← render 16ms 是唯一瓶颈
`

**实时交互已到天花板**：时间/温度/光照这类局部刷新每块仅 38.4KB，
一帧内即可完成（等效 100fps 以上）；只有**整屏重绘**（如切页动画）受 58fps 约束。

### 若要继续压（需做取舍）

1. **样式减肥**：主题默认给对象叠加圆角/渐变/变换，逐对象置平可省 2~4ms，
   代价是界面变"平"（少圆角/无渐变）。
2. **总线超规格**：15MHz → 20MHz，天花板 133fps；但已不是总线瓶颈，收益仅限
   "总线窗口更宽松"，约 +3~5fps，且超出 ST7789 写周期规格（66ns）需示波器确认。
3. **换渲染策略**：局部增量 + 控件级脏区（现已是默认行为），或把大字号数字改成
   位图数字（避免每帧 AA 字形混合）。
