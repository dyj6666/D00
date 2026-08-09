param(
    [int]$Seconds = 25,
    [string]$Port = "SWD",
    [switch]$NoReset
)
. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $script:LogDir | Out-Null

$report = New-Report
$report.git = Get-GitHead

Write-Step ("Capture " + $script:DebugPort + " for " + $Seconds + "s, reset via SWD")
$logger = Start-Com9Logger -OutFile $script:BootLog -Seconds $Seconds
Start-Sleep -Seconds 2
if (-not $NoReset) {
    $r = Invoke-Exe -FilePath $script:Programmer -Arguments @("-c", "port=$Port", "-rst") -TimeoutSec 60
    if ($r.ExitCode -ne 0) {
        Stop-Logger $logger
        throw "SWD reset failed - is the target connected?"
    }
}
try { $logger.WaitForExit(($Seconds + 30) * 1000) } catch {}
Stop-Logger $logger

$txt = ""
if (Test-Path -LiteralPath $script:BootLog) { $txt = Get-Content -LiteralPath $script:BootLog -Raw }
$v = Test-BootLog $txt

$report.stages["verify"] = $(if ($v.Pass) { "OK" } else { "FAIL" })
$report.stages["verify_log"] = $script:BootLog
$report.stages["verify_missing"] = $v.Missing
$report.stages["verify_bad"] = $v.Bad
if ($v.Recovered.Count -gt 0) {
    $report.stages["verify_recovered_crash"] = (($v.Recovered | Select-Object -Last 3) -join " | ")
    Write-Host ("WARN: previous-crash recovery recorded on boot: " + ($v.Recovered | Select-Object -Last 1))
}
$report.stages["verify_tail"] = (($txt -split "`r?`n" | Select-Object -Last 25) -join "`n")
$report.generated = Get-Date -Format "o"
Save-Report $report

if ($v.Missing.Count -gt 0) { Write-Host ("MISSING markers: " + ($v.Missing -join ", ")) }
if ($v.Bad.Count -gt 0) { Write-Host ("CRASH/BAD markers: " + ($v.Bad -join ", ")) }
if (-not $v.Pass) {
    Write-Host "VERIFY FAILED"
    exit 1
}
Write-Host "VERIFY OK: boot log shows APP fully started"
exit 0
