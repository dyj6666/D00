# ============================================================
# D00 一键安装 / 环境准备
#   1. 启用 git hooks（pre-commit 编码/卫生检查）
# ============================================================
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path

git -C $Root config core.hooksPath .githooks
Write-Host "[OK] git hooks enabled (.githooks)"

if (Test-Path -LiteralPath (Join-Path $Root "workflow\common.ps1")) {
    Write-Host "[OK] AI workflow ready: $Root\workflow"
} else {
    Write-Host "[WARN] workflow 目录缺失（请确认已完整克隆仓库）"
}
Write-Host "[OK] 环境准备完成"
