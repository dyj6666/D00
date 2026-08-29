# 提权脚本：备份 hosts → 移除 GitHub 屏蔽条目 → 刷新 DNS（由 UAC 提权运行）
$ErrorActionPreference = 'Stop'
$hosts = 'C:\Windows\System32\drivers\etc\hosts'
$bak = "$hosts.bak_github_unblock"
if (-not (Test-Path $bak)) { Copy-Item $hosts $bak -Force }
$lines = Get-Content $hosts
$removed = @($lines | Where-Object { $_ -match 'github' })
$new = @($lines | Where-Object { $_ -notmatch 'github' })
Set-Content -Path $hosts -Value $new -Encoding ASCII
ipconfig /flushdns | Out-Null
"removed=$($removed.Count) lines, hosts size=$($new.Count)" | Out-File -Path 'D:\GIT-SPACE\D00\_notes\hosts_fix_result.txt' -Encoding UTF8
