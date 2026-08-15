param(
    [int]$Version = 0,
    [int]$BuildNo = 0,
    [string]$Port = "",
    [int]$CaptureSeconds = 35,
    [string]$Ip = "192.168.10.10",
    [switch]$Tcp
)
. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $script:LogDir | Out-Null

$report = New-Report
$report.git = Get-GitHead
if (-not $Port) {
    if ($Tcp) { $Port = "" }
    else { $Port = Get-HostPort }
}
if ($Tcp -and -not $Port) { $Port = "" }
Assert-File $script:AppBin "APP.bin (run auto_build first)"

$v = $(if ($Version -gt 0) { $Version } else { $script:OtaVersion })
$b = $(if ($BuildNo -gt 0) { $BuildNo } else { $script:OtaBuildNo })
$debugLog = Join-Path $script:AppRoot "_auto_ota_debug.txt"
$otaLog = Join-Path $script:LogDir "ota_hostlink.log"

if ($Tcp) {
    Write-Step ("OTA smoke(TCP): version=" + $v + " build=" + $b + " ip=" + $Ip)
    $logger = Start-Com9Logger -OutFile $debugLog -Seconds $CaptureSeconds
    Start-Sleep -Seconds 2
    $r = Invoke-Exe -FilePath $script:Python -Arguments @($script:OtaTcpCli, $script:AppBin, "$v", "$b", $Ip) -LogFile $otaLog -TimeoutSec 900
} else {
    Write-Step ("OTA smoke(HOSTLINK): version=" + $v + " build=" + $b + " port=" + $Port)
    $logger = Start-Com9Logger -OutFile $debugLog -Seconds $CaptureSeconds
    Start-Sleep -Seconds 2
    $r = Invoke-Exe -FilePath $script:Python -Arguments @($script:OtaCli, "--no-resume", $script:AppBin, "$v", "$b", $Port) -LogFile $otaLog -TimeoutSec 900
}
try { $logger.WaitForExit(($CaptureSeconds + 30) * 1000) } catch {}
Stop-Logger $logger

$otaOk = ($r.ExitCode -eq 0) -and ($r.Stdout -notmatch "FAILED|no response|err=[1-9]|state=[2-9]")
$hostline = (($r.Stdout -split "`r?`n") | Select-String -Pattern "OTA|BOOT|phase|FAIL" | Select-Object -Last 15) -join "`n"
$txt = ""
if (Test-Path -LiteralPath $debugLog) { $txt = Get-Content -LiteralPath $debugLog -Raw }
$v2 = Test-BootLog $txt
$pass = $otaOk -and $v2.Pass

$report.stages["ota_download"] = $(if ($otaOk) { "OK" } else { "FAIL" })
$report.stages["ota_verify"] = $(if ($v2.Pass) { "OK" } else { "FAIL" })
if ($v2.Recovered.Count -gt 0) {
    $report.stages["ota_recovered_crash"] = (($v2.Recovered | Select-Object -Last 1) -join " | ")
}
$report.stages["ota_log"] = $otaLog
$report.stages["ota_debug_log"] = $debugLog
$report.stages["ota_host_tail"] = $hostline
$report.generated = Get-Date -Format "o"
Save-Report $report

if (-not $pass) {
    Write-Host $hostline
    if (-not $otaOk) { Write-Host "OTA download reported failure" }
    if (-not $v2.Pass) { Write-Host ("BOOT LOG MISSING markers: " + ($v2.Missing -join ", ")) }
    Write-Host "OTA STAGE FAILED"
    exit 1
}
Write-Host "OTA OK: secure package downloaded, BOOT verified, APP booted"
exit 0
