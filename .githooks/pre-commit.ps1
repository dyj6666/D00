# pre-commit：暂存区文件编码/行尾卫生检查（UTF-8 + LF + 末尾换行）
$ErrorActionPreference = "Stop"
$Repo = git rev-parse --show-toplevel
$Staged = git -C $Repo diff --cached --name-only --diff-filter=ACM

$Bad = New-Object System.Collections.ArrayList
foreach ($File in $Staged) {
    $Path = Join-Path $Repo $File
    if (-not (Test-Path -LiteralPath $Path)) { continue }
    $Ext = [System.IO.Path]::GetExtension($File)
    if ($Ext -notin @('.c', '.h', '.py', '.ps1', '.md', '.json', '.txt', '.yml', '.yaml', '.cmake')) { continue }
    $Bytes = [System.IO.File]::ReadAllBytes($Path)
    try {
        $null = [System.Text.UTF8Encoding]::new($false, $true).GetString($Bytes)
    } catch {
        [void]$Bad.Add("$File : 非 UTF-8 编码")
    }
    if ($Bytes.Length -gt 0 -and $Bytes[-1] -ne 0x0A) {
        [void]$Bad.Add("$File : 缺少末尾换行")
    }
    $Text = [System.Text.Encoding]::UTF8.GetString($Bytes)
    if ($Text -match "(?m)[ \t]+$") {
        [void]$Bad.Add("$File : 存在行尾空白")
    }
}

if ($Bad.Count -gt 0) {
    Write-Host "pre-commit FAIL:"
    $Bad | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
Write-Host "pre-commit OK"
