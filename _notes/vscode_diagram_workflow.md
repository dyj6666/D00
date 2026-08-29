# VS Code 知识结构图工作台 — 顶级工作流手册

> 环境：VS Code 1.133 + 图表插件 5 件套 + 网页 HTML 查看器
> 配套文件：`linux_kernel_structure.md`（主文档）· `.mmd`（源码）· `.html`（展示）· `.svg/.png`（矢量/位图）

---

## 1. 工具栈总览

| 工具 | 角色 | 触发方式 |
| --- | --- | --- |
| **MPE**（shd101wyy.markdown-preview-enhanced） | 主预览引擎：渲染 Mermaid、导出 PDF/HTML | `Ctrl+Shift+V` |
| **Mermaid Editor**（tomoyukim.vscode-mermaid-editor） | 可视化编辑：实时预览、拖拽微调、导出 | `Ctrl+Shift+P` → `Mermaid Editor: Open` |
| **Markmap**（gera2ld.markmap-vscode） | Markdown 大纲 → 思维导图 | `Ctrl+K M` |
| **Draw.io**（hediet.vscode-drawio） | 自由画布流程图（.drawio） | 命令面板 `Draw.io: Open` |
| **markdown-mermaid**（bierner） | 内置预览的 Mermaid 备用渲染器 | 随预览生效 |
| **HTML 查看器**（`linux_kernel_structure.html`） | 展示/分享：内嵌 SVG 离线可用 + CDN 实时 Mermaid | 浏览器双击 |

## 2. 文件角色约定

| 文件 | 角色 | 谁编辑 |
| --- | --- | --- |
| `*.md` | **主文档**：图 + 说明文字，进 git 版本管理 | 文本/Mermaid Editor |
| `*.mmd` | **纯图源码**：Mermaid 代码 | Mermaid Editor / 文本 |
| `*.drawio` | 自由画布图 | Draw.io 插件 |
| `*.html` | 展示/分享（自包含） | 生成脚本 / 手工 |

## 3. 查看工作流

```
打开 .md → Ctrl+Shift+V          ← 日常最快（暗色主题渲染）
打开 .md → Ctrl+K M              ← 结构总览/导航（思维导图，可折叠）
双击 .html（Edge）                ← 分享/演示（不依赖 VS Code）
```

## 4. 编辑工作流

**① 文本编辑（推荐，可版本化）**
改 `.md` 里 ```mermaid``` 代码块 → 预览实时刷新。适合结构调整、文字修正。
进阶：`.mmd` 保持"纯图"和文档分离，图改完再嵌入文档。

**② 可视化编辑（精修）**
1. 打开 `linux_kernel_structure.mmd`
2. `Ctrl+Shift+P` → `Mermaid Editor: Open`
3. 左改右看；预览区**拖拽微调**节点位置
4. 工具栏导出 PNG / SVG / PDF

**③ 思维导图式编辑（重构大纲）**
`Ctrl+K M` 打开 Markmap → 在 `.md` 里调整标题层级（#/##/###）→ 导图实时重构。
适合"先搭骨架再细化"：大纲即结构，结构即文档。

**④ 自由画布（从零创作）**
新建 `xxx.drawio` → `Draw.io: Open` → 拖拽绘制（形状/连线/颜色）→ `Ctrl+S` 存盘即文本格式，可进 git。

## 5. 导出矩阵

| 目标格式 | 工具 | 操作 |
| --- | --- | --- |
| PNG / SVG | Mermaid Editor | 编辑面板工具栏一键导出 |
| SVG / PNG / HTML | Markmap | 命令面板 `Markmap: Export` |
| PDF | MPE | 预览中右键 → Export to PDF（或命令面板） |
| HTML | Markmap / 自建 | 导图导出 HTML；或复用 `gen_final_diagram.py` 生成自包含 HTML |

## 6. 快捷键速查

| 按键 | 功能 |
| --- | --- |
| `Ctrl+Shift+V` | Markdown 预览 |
| `Ctrl+K V` | 侧边预览 |
| `Ctrl+K M` | Markmap 思维导图视图 |
| `Ctrl+Shift+P` → `Mermaid Editor: Open` | Mermaid 可视化编辑器 |
| `Ctrl+Shift+P` → `Draw.io: Open` | 打开 .drawio 画布 |

## 7. 环境要点（已配置好）

- **暗色渲染**：`markdown-mermaid.darkModeTheme: "dark"` + `markdown-preview-enhanced.mermaidTheme: "dark"`（用户设置）
- **黑屏修复**：`window.disableHardwareAcceleration: true`（GPU WebView 崩溃导致预览全黑）
- **推荐清单**：`.vscode/extensions.json`（新机器一键安装提示）
- **批量安装**：`_notes/setup_vscode_exts.ps1`
- **图源码级兜底**：如遇主题异常，可在 mermaid 首行加 `%%{init: {"theme": "dark"}}%%`

## 8. 常见问题（FAQ）

| 现象 | 原因 | 解决 |
| --- | --- | --- |
| 预览整块黑、闪一下消失 | WebView GPU 崩溃 | 确认 `disableHardwareAcceleration: true` 后**完全重启** VS Code |
| 图是白色/浅色 | 主题设置被覆盖 | 用户设置里 `darkModeTheme`/`mermaidTheme` 改为 `dark`，Reload Window |
| 图不渲染只显示代码 | 预览引擎未接管 | 确认 MPE 已装；`Ctrl+Shift+V` 由 MPE 打开 |
| 中文显示为方块 | 字体缺失 | Windows 默认雅黑正常；Linux 需安装 `fonts-noto-cjk` |

## 9. 一条龙示例（以本仓库内核图为例）

```text
1. 看结构     → 打开 linux_kernel_structure.md，Ctrl+Shift+V
2. 看导图     → Ctrl+K M
3. 精修图     → 打开 .mmd，Mermaid Editor: Open，拖拽+导出
4. 改大纲     → 在 .md 里调整 #/## 层级，Markmap 实时更新
5. 分享出去   → 双击 linux_kernel_structure.html（Edge）
6. 回归 git   → 只提交 .md/.mmd（构建产物与图片不入库）
```
