param([switch]$TestHw)
. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = "Continue"

$fails = New-Object System.Collections.ArrayList
$warns = New-Object System.Collections.ArrayList

function Report {
    param([bool]$Ok, [string]$Name, [string]$Detail = "")
    if ($Ok) {
        Write-Host ("[OK]   " + $Name)
    } else {
        Write-Host ("[FAIL] " + $Name + $(if ($Detail) { "  -> " + $Detail } else { "" }))
        [void]$fails.Add($Name)
    }
}
function Warn {
    param([string]$Name, [string]$Detail)
    Write-Host ("[WARN] " + $Name + "  -> " + $Detail)
    [void]$warns.Add($Name)
}

Write-Step "D00 environment self-check (repo: $script:RepoRoot)"
Report (Test-Path -LiteralPath $script:UV4) "Keil UV4" $script:UV4
Report (Test-Path -LiteralPath $script:Programmer) "STM32_Programmer_CLI" $script:Programmer
Report (Test-Path -LiteralPath $script:Python) "Python" $script:Python
if (Test-Path -LiteralPath $script:Python) {
    $r = Invoke-Exe -FilePath $script:Python -Arguments @("-c", "import serial; print('pyserial ok')") -TimeoutSec 60
    Report ($r.ExitCode -eq 0) "Python pyserial" $r.Stdout.Trim()
}
$cm = Get-Command cmake -ErrorAction SilentlyContinue
Report ($null -ne $cm) "cmake" $(if ($cm) { $cm.Source } else { "not in PATH" })
$nm = Get-Command ninja -ErrorAction SilentlyContinue
Report ($null -ne $nm) "ninja" $(if ($nm) { $nm.Source } else { "not in PATH" })
$ocd = $script:OpenOCD
Report (Test-Path -LiteralPath $ocd) "OpenOCD (CMSIS-DAP)" $(if (Test-Path -LiteralPath $ocd) { $ocd } else { "not found - DAP flash/reset unavailable" })
Report (Test-Path -LiteralPath (Join-Path $script:RepoRoot ".git")) "git repo" $script:RepoRoot
Report (Test-Path -LiteralPath $script:AppUvprojx) "APP project" $script:AppUvprojx
Report (Test-Path -LiteralPath $script:BootUvprojx) "BOOT project" $script:BootUvprojx

try {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
} catch {
    $ports = @()
}
if ($ports -notcontains $script:DebugPort) { Warn "DebugPort" ("{0} not present (device unplugged?)" -f $script:DebugPort) }
if ($ports -notcontains $script:HostPort) { Warn "HostPort" ("{0} not present (device unplugged?)" -f $script:HostPort) }

# Conventions consistency: key literals must match the firmware headers.
$appCfg = Join-Path $script:AppRoot "Config\app_config.h"
$bootCfg = Join-Path $script:BootRoot "Config\boot_config.h"
if (Test-Path -LiteralPath $appCfg) {
    $txt = Get-Content -LiteralPath $appCfg -Raw
    if ($txt -notmatch "0x080DFFFC") { Warn "app_config.h" "OTA_APP_VERSION_ADDR 0x080DFFFC not found (convention drift?)" }
    if ($txt -notmatch "1024 \* 1024") { Warn "app_config.h" "OTA_EXT_DL_SIZE 1MB not found (Plan-B drift?)" }
} else { Warn "app_config.h" "not found" }
if (Test-Path -LiteralPath $bootCfg) {
    $txt = Get-Content -LiteralPath $bootCfg -Raw
    if ($txt -notmatch "0x4F54412E") { Warn "boot_config.h" "APP_VALID_MAGIC 0x4F54412E not found (convention drift?)" }
} else { Warn "boot_config.h" "not found" }

# ---------- 版本单一事实源：config/version.json 与 common.ps1 必须一致 ----------
# ---------- Project file hygiene: BOM / stale objects ----------
Write-Step "Project file hygiene check"
$projFiles = @(
    (Join-Path $script:AppRoot "MDK-ARM\APP.uvprojx"),
    (Join-Path $script:AppRoot "MDK-ARM\APP\APP.sct"),
    (Join-Path $script:AppRoot "cmake\APP.ld"),
    (Join-Path $script:BootRoot "MDK-ARM\BOOT.uvprojx"),
    (Join-Path $script:BootRoot "MDK-ARM\BOOT\BOOT.sct")
)
$bomBad = Test-ProjectNoBom -Files $projFiles
foreach ($f in $bomBad) { Warn "BOM" ("UTF-8 BOM found in Keil project file (breaks Keil): " + $f) }
if ($bomBad.Count -eq 0) { Report $true "No BOM in project files" "OK" }

if (Test-StaleObjects -ProjectDir $script:AppRoot) {
    Warn "StaleObjects" "Key config header newer than some .o - run auto_build -Clean for a full rebuild"
} else {
    Report $true "Incremental build hygiene" "OK"
}

Write-Step "Version single-source check"
$verFile = Join-Path $script:RepoRoot "config\version.json"
if (Test-Path -LiteralPath $verFile) {
    try {
        $ver = Get-Content -LiteralPath $verFile -Raw | ConvertFrom-Json
        $verMatch = ([int]$ver.ota_version -eq $script:OtaVersion) -and
                    ([int]$ver.ota_build -eq $script:OtaBuildNo)
        Report $verMatch "config/version.json vs common.ps1" `
            ("v=" + $script:OtaVersion + " b=" + $script:OtaBuildNo)
    } catch {
        Report $false "config/version.json" "parse failed"
    }
} else {
    Report $false "config/version.json" "missing (single source of truth)"
}

# ---------- 发布固件安全：不得含崩溃注入后门 ----------
Write-Step "Release firmware security check"
if (Test-Path -LiteralPath $script:AppBin) {
    $appTxt = [System.Text.Encoding]::ASCII.GetString(
        [System.IO.File]::ReadAllBytes($script:AppBin))
    $noBackdoor = -not $appTxt.Contains("Crash injection")
    Report $noBackdoor "APP.bin no crash-injection backdoor" `
        $(if ($noBackdoor) { "" } else { $script:AppBin })
} else {
    Warn "APP.bin" "not built yet (crash-backdoor scan skipped)"
}

# ---------- 源码编码：全部自有源文件必须为合法 UTF-8 ----------
Write-Step "Source encoding (UTF-8) check"
$encRoots = @(
    (Join-Path $script:RepoRoot "APP\APP\Application"),
    (Join-Path $script:RepoRoot "APP\APP\SystemServices"),
    (Join-Path $script:RepoRoot "APP\APP\BSP"),
    (Join-Path $script:RepoRoot "APP\APP\Config"),
    (Join-Path $script:RepoRoot "APP\APP\Doc"),
    (Join-Path $script:RepoRoot "APP\APP\Script"),
    (Join-Path $script:RepoRoot "APP\APP\tests"),
    (Join-Path $script:RepoRoot "APP\APP\cmake"),
    (Join-Path $script:RepoRoot "BOOT\BOOT\BootApp"),
    (Join-Path $script:RepoRoot "BOOT\BOOT\BootServices"),
    (Join-Path $script:RepoRoot "BOOT\BOOT\Config"),
    (Join-Path $script:RepoRoot "BOOT\BOOT\Core"),
    (Join-Path $script:RepoRoot "BOOT\BOOT\tests"),
    (Join-Path $script:RepoRoot "HOST"),
    (Join-Path $script:RepoRoot "workflow")
)
$encBad = New-Object System.Collections.ArrayList
$strictUtf8 = New-Object System.Text.UTF8Encoding($false, $true)
foreach ($d in $encRoots) {
    if (-not (Test-Path -LiteralPath $d)) { continue }
    Get-ChildItem -LiteralPath $d -Recurse -File | ForEach-Object {
        if ($_.FullName -match '\\(build|__pycache__|MDK-ARM|Middlewares|Drivers)\\') { return }
        if ($_.Extension -notin @(".c", ".h", ".py", ".ps1", ".md", ".json")) { return }
        try {
            $null = $strictUtf8.GetString([System.IO.File]::ReadAllBytes($_.FullName))
        } catch {
            [void]$encBad.Add($_.FullName)
        }
    }
}
Report ($encBad.Count -eq 0) "Source encoding UTF-8" `
    $(if ($encBad.Count -gt 0) { $encBad[0] } else { "" })

# ---------- 可复现性：GCC 构建文件 / CI / 工作流必须入库 ----------
Write-Step "Reproducibility check"
Report (Test-Path -LiteralPath (Join-Path $script:AppRoot "cmake\APP.ld")) "APP.ld"
Report (Test-Path -LiteralPath (Join-Path $script:AppRoot "cmake\arm-none-eabi-toolchain.cmake")) "APP GCC toolchain"
Report (Test-Path -LiteralPath (Join-Path $script:RepoRoot ".github\workflows\ci.yml")) "root CI workflow"
& git -C $script:RepoRoot ls-files --error-unmatch AGENTS.md workflow/common.ps1 config/version.json 2>$null | Out-Null
Report ($LASTEXITCODE -eq 0) "workflow versioned in git"

# ---------- 文档漂移：README 关键约定必须与现状一致 ----------
$readmePath = Join-Path $script:RepoRoot "README.md"
if (Test-Path -LiteralPath $readmePath) {
    $readmeTxt = Get-Content -LiteralPath $readmePath -Raw
    Report ($readmeTxt -match "0x0805FFF8" -and $readmeTxt -match "320KB") `
        "README conventions" "0x0805FFF8 / APP 320KB"
}

if (Test-Path -LiteralPath (Join-Path $script:RepoRoot ".git")) {
    try {
        $branch = (git -C $script:RepoRoot rev-parse --abbrev-ref HEAD 2>$null | Select-Object -First 1)
        $dirty = (git -C $script:RepoRoot status --porcelain 2>$null | Measure-Object).Count
        Write-Host ("[INFO] git branch=" + $branch + " dirty=" + $dirty)
    } catch {}
}

if ($TestHw) {
    Write-Step "Probing target (DAP -> ST-Link fallback, reset only)"
    try {
        $m = Reset-Target
        Report $true "Target probe" "reset via $m"
    } catch {
        Report $false "Target probe" $_.Exception.Message
    }
}

Write-Host ""
if ($fails.Count -gt 0) { Write-Host ("FAILED: " + ($fails -join ", ")); exit 1 }
if ($warns.Count -gt 0) { Write-Host ("PASSED with warnings: " + ($warns -join ", ")); exit 0 }
Write-Host "ALL CHECKS PASSED"
exit 0
