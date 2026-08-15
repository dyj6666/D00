# ============================================================
# ota_tcp_push - Ethernet TCP OTA push (network channel wrapper)
#
# Wraps ota_tcp_cli.py with automatic version/build from
# config/version.json. TCP OTA requires network permission on the
# host (LAN to the board, e.g. 192.168.10.10:9020).
#
# Usage:
#   powershell -File workflow\ota_tcp_push.ps1 [-Ip 192.168.10.10] [-Version N] [-BuildNo N]
# ============================================================
param(
    [string]$Ip = "192.168.10.10",
    [int]$Version = 0,
    [int]$BuildNo = 0
)
. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = "Stop"

Assert-File $script:AppBin "APP.bin (run auto_build first)"
Assert-File $script:OtaTcpCli "ota_tcp_cli.py"

$v = $(if ($Version -gt 0) { $Version } else { $script:OtaVersion })
$b = $(if ($BuildNo -gt 0) { $BuildNo } else { $script:OtaBuildNo })
Write-Step ("TCP OTA push: v" + $v + " build " + $b + " -> " + $Ip + ":9020")

$r = Invoke-Exe -FilePath $script:Python -Arguments @(
    $script:OtaTcpCli, $script:AppBin, "$v", "$b", $Ip) -TimeoutSec 900

$ok = ($r.ExitCode -eq 0) -and ($r.Stdout -notmatch "FAILED|no response|BEGIN FAILED|err=[1-9]")
Write-Host (($r.Stdout -split "`r?`n" | Select-Object -Last 8) -join "`n")
if (-not $ok) {
    Write-Host "TCP OTA PUSH FAILED (check board IP/ETH link and firewall)"
    exit 1
}
Write-Host "TCP OTA PUSH OK - verify BOOT/APP boot log separately"
exit 0
