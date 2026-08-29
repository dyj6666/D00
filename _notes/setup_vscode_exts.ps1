# 一键安装 VS Code 图表/知识结构图插件（需网络）
# 用法：pwsh -File setup_vscode_exts.ps1 （或右键 -> 使用 PowerShell 运行）
$exts = @(
  'bierner.markdown-mermaid',            # Markdown 中渲染 Mermaid（核心，一般已装）
  'tomoyukim.vscode-mermaid-editor',    # Mermaid 可视化编辑器：实时预览 / 导出 PNG·SVG·PDF
  'gera2ld.markmap-vscode',             # Markdown 大纲 -> 思维导图（知识结构图神器）
  'hediet.vscode-drawio',               # 内嵌编辑 .drawio 图表
  'shd101wyy.markdown-preview-enhanced' # 增强预览：Mermaid/PlantUML/流程图，导出 PDF/HTML
)
foreach ($e in $exts) {
  Write-Host "==> 安装 $e"
  code --install-extension $e --force
  if ($LASTEXITCODE -eq 0) { Write-Host "    完成" } else { Write-Host "    失败（检查网络后重试）" }
}
Write-Host "`n全部结束，重启 VS Code 生效。"
