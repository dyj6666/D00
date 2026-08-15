param(
    [switch]$SkipBoot,
    [switch]$SkipApp,
    [switch]$KeepImage,
    [int]$Version = 0,
    [string]$Port = "SWD",
    [switch]$Dap
)
. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $script:LogDir | Out-Null

$report = New-Report
$report.git = Get-GitHead
$flashLog = Join-Path $script:LogDir "flash.log"

if (-not $SkipBoot) { Assert-File $script:BootBin "BOOT.bin (run auto_build first)" }
if (-not $SkipApp) {
    Assert-File $script:AppBin "APP.bin (run auto_build first)"
    if ($KeepImage -and (Test-Path -LiteralPath $script:AppFlashImg)) {
        Write-Step "Reuse existing APP_flash.bin"
    } else {
        Write-Step "Generate APP partition image (magic + version)"
        $ver = $(if ($Version -gt 0) { $Version } else { $script:OtaVersion })
        $r = Invoke-Exe -FilePath $script:Python -Arguments @($script:AppMagicPy, $script:AppBin, "--version", "$ver", "--out", $script:AppFlashImg) -LogFile $flashLog -TimeoutSec 120
        if ($r.ExitCode -ne 0) {
            Write-Host $r.Stdout
            throw "append_app_magic failed"
        }
    }
}

$cmdArgs = @("-c", "port=$Port", "mode=UR")
if (-not $SkipBoot) {
    $cmdArgs += @("-e", $script:BootSectors)
    $cmdArgs += @("-d", $script:BootBin, $script:BootAddr, "-v")
}
if (-not $SkipApp) {
    $cmdArgs += @("-e", $script:AppSectors)
    $cmdArgs += @("-d", $script:AppFlashImg, $script:AppAddr, "-v")
}
$cmdArgs += @("-rst")

if ($Dap) {
    # CMSIS-DAP ???flash_dap.ps1?500kHz + ?? + ???? DAP ???
    Write-Step "DAP flash: BOOT+APP via flash_dap.ps1"
    $ok = $true
    if (-not $SkipBoot) {
        $r = Invoke-Exe -FilePath "powershell.exe" -Arguments @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $script:DapFlash,
            "-File", $script:BootBin, "-Addr", "0x08000000") -LogFile $flashLog -TimeoutSec 600
        if ($r.ExitCode -ne 0) { $ok = $false; $okDetail = "BOOT DAP flash failed" }
    }
    if ($ok -and -not $SkipApp) {
        $r = Invoke-Exe -FilePath "powershell.exe" -Arguments @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $script:DapFlash,
            "-File", $script:AppFlashImg, "-Addr", "0x08010000") -LogFile $flashLog -TimeoutSec 900
        $ok = ($r.ExitCode -eq 0)
        if (-not $ok) { $okDetail = "APP DAP flash failed" }
    }
    if ($ok) { $okDetail = "DAP flash + verify OK" }
} else {
    Write-Step "STM32CubeProgrammer: erase + program + verify + reset"
    $r = Invoke-Exe -FilePath $script:Programmer -Arguments $cmdArgs -LogFile $flashLog -TimeoutSec 600
    $ok = $r.ExitCode -eq 0
    $okDetail = "STM32_Programmer"
}
$ok = [bool]$ok
$report.stages["flash"] = $(if ($ok) { "OK" } else { "FAIL" })
$report.stages["flash_log"] = $flashLog
$report.stages["flash_targets"] = [pscustomobject]@{
    boot = $(if ($SkipBoot) { "skip" } else { $script:BootAddr })
    app  = $(if ($SkipApp) { "skip" } else { $script:AppAddr })
}
$report.generated = Get-Date -Format "o"
Save-Report $report

if (-not $ok) {
    Write-Host ("FLASH FAILED (see " + $flashLog + ")")
    if (Test-Path -LiteralPath $flashLog) {
        Get-Content -LiteralPath $flashLog -Tail 30 | ForEach-Object { Write-Host $_ }
    }
    exit 1
}
Write-Host "FLASH OK: $okDetail"
exit 0
