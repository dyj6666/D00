# ============================================================
# D00 embedded automation - shared config + helpers (ASCII only)
# Set $env:D00_REPO_ROOT to override the detected repo root.
# ============================================================

$script:WorkflowDir = $PSScriptRoot
$script:RepoRoot = if ($env:D00_REPO_ROOT) { $env:D00_REPO_ROOT } else { Split-Path -Parent $PSScriptRoot }
$script:AppRoot  = Join-Path $RepoRoot "APP\APP"
$script:BootRoot = Join-Path $RepoRoot "BOOT\BOOT"
$script:HostRoot = Join-Path $RepoRoot "HOST"

$script:UV4        = "D:\MDK\CORE\UV4\UV4.exe"
$script:Programmer = "D:\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
$script:Python     = "D:\Python\python.exe"
$script:Cmake      = "cmake"
$script:Ninja      = "ninja"

$script:AppUvprojx   = Join-Path $AppRoot "MDK-ARM\APP.uvprojx"
$script:BootUvprojx  = Join-Path $BootRoot "MDK-ARM\BOOT.uvprojx"
$script:AppBin       = Join-Path $AppRoot "MDK-ARM\Output\APP.bin"
$script:BootBin      = Join-Path $BootRoot "MDK-ARM\BOOT.bin"
$script:AppFlashImg  = Join-Path $AppRoot "MDK-ARM\Output\APP_flash.bin"
$script:AppGccBin    = Join-Path $AppRoot "build-cmake\APP.bin"
$script:BootGccBin   = Join-Path $BootRoot "build-fw\BOOT.bin"
$script:AppMagicPy   = Join-Path $AppRoot "Script\append_app_magic.py"
$script:Com9Logger   = Join-Path $AppRoot "Script\com9_logger.py"
$script:OtaCli       = Join-Path $AppRoot "Script\ota_hostlink_cli.py"
$script:CmakeBuildPs1 = Join-Path $AppRoot "Script\cmake_build.ps1"

$script:BootAddr    = "0x08000000"
$script:AppAddr     = "0x08010000"
$script:BootSectors = @("0","1","2","3")   # BOOT 64KB = 扇区 0-3（与 boot_config.h 一致）
$script:AppSectors  = @("4","5","6")

$script:DebugPort = if ($env:D00_DEBUG_PORT) { $env:D00_DEBUG_PORT } else { "COM5" }
$script:HostPort  = if ($env:D00_HOST_PORT)  { $env:D00_HOST_PORT  } else { "COM13" }
$script:BootLog   = Join-Path $AppRoot "_auto_boot.txt"
$script:OtaLog    = Join-Path $AppRoot "_auto_ota.txt"

# Version written next to the APP validity magic (0x0805FFFC).
# Defaults live in config/version.json (single source of truth) and
# override these fallback values; keep both consistent before release.
$script:OtaVersion = 201
$script:OtaBuildNo = 9077
$script:VersionFile = Join-Path $RepoRoot "config\version.json"
if (Test-Path -LiteralPath $script:VersionFile) {
    try {
        $v = Get-Content -LiteralPath $script:VersionFile -Raw | ConvertFrom-Json
        if ($v.ota_version -gt 0) { $script:OtaVersion = [int]$v.ota_version }
        if ($v.ota_build -gt 0)   { $script:OtaBuildNo = [int]$v.ota_build }
    } catch {
        Write-Warning "config/version.json parse failed, using defaults"
    }
}

$script:LogDir      = Join-Path $WorkflowDir "logs"
$script:ReportPath  = Join-Path $WorkflowDir "last_report.json"

$script:VerifyExpect = @("Modules initialized", "ETH  : app ready", "OTA  : Agent ready")
$script:VerifyFail   = @("HardFault", "UsageFault", "assert", "FATAL")

function Write-Step {
    param([string]$Msg)
    Write-Host ("[workflow] " + $Msg)
}

function Assert-File {
    param([string]$Path, [string]$Desc)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw ($Desc + " not found: " + $Path)
    }
}

function New-Report {
    [ordered]@{
        tool      = "d00-auto"
        generated = (Get-Date -Format "o")
        git       = ""
        stages    = [ordered]@{}
    }
}

function Save-Report {
    param($Report)
    $Report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $script:ReportPath -Encoding UTF8
}

function Get-GitHead {
    try {
        $r = git -C $script:RepoRoot rev-parse --short HEAD 2>$null
        if ($LASTEXITCODE -eq 0) { return ($r | Select-Object -First 1) }
    } catch {}
    return "unknown"
}

function Get-FileSha256 {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return "" }
    try {
        return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    } catch { return "" }
}

function Invoke-Exe {
    param(
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$LogFile = "",
        [int]$TimeoutSec = 600
    )
    $ErrorActionPreference = "Stop"
    $resolved = $FilePath
    if (-not (Test-Path -LiteralPath $FilePath)) {
        $cmd = Get-Command $FilePath -ErrorAction SilentlyContinue
        if ($cmd) { $resolved = $cmd.Source } else { throw "Executable not found: $FilePath" }
    }
    $argLine = ($Arguments | ForEach-Object { if ($_ -match "\s") { '"' + $_ + '"' } else { $_ } }) -join " "
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $resolved
    $psi.Arguments = $argLine
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    try {
        $exited = $p.WaitForExit($TimeoutSec * 1000)
    } catch {
        $exited = $false
    }
    if (-not $exited) {
        try { $p.Kill() } catch {}
        throw "Timed out after ${TimeoutSec}s: $resolved"
    }
    try { $p.WaitForExit() } catch {}
    try { $stdout = $outTask.Result } catch { $stdout = "" }
    try { $stderr = $errTask.Result } catch { $stderr = "" }
    if ($LogFile) {
        $combined = $stdout
        if ($stderr) { $combined += "`n" + $stderr }
        if ($combined) { Set-Content -LiteralPath $LogFile -Value $combined -Encoding UTF8 }
    }
    [pscustomobject]@{ ExitCode = $p.ExitCode; Stdout = $stdout; Stderr = $stderr }
}

function Invoke-UV4 {
    param(
        [string]$Project,
        [string]$LogFile,
        [int]$TimeoutSec = 300,
        [switch]$Rebuild
    )
    $ErrorActionPreference = "Stop"
    Assert-File $Project "UV4 project"
    Remove-Item -LiteralPath $LogFile -Force -ErrorAction SilentlyContinue
    # Default: incremental build (-b). -Rebuild forces full rebuild (-r).
    $mode = "-b"
    if ($Rebuild) { $mode = "-r" }
    $p = Start-Process -FilePath $script:UV4 -ArgumentList @("-j0", $mode, $Project, "-o", $LogFile) `
        -WindowStyle Hidden -PassThru
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $p.Refresh()
        if ($p.HasExited) { break }
        Start-Sleep -Seconds 5
    }
    $p.Refresh()
    if (-not $p.HasExited) {
        throw "UV4 build timed out (is Keil IDE open?). Log: $LogFile"
    }
    [pscustomobject]@{ ExitCode = $p.ExitCode; Log = $LogFile }
}

function Test-KeilLog {
    param([string]$LogFile)
    $txt = ""
    if (Test-Path -LiteralPath $LogFile) { $txt = Get-Content -LiteralPath $LogFile -Raw }
    $ok = $txt -match "0 Error\(s\)"
    $warnings = 0
    if ($txt -match "(\d+)\s+Warning\(s\)") { $warnings = [int]$Matches[1] }
    [pscustomobject]@{ Ok = $ok; Warnings = $warnings; Text = $txt }
}

function Start-Com9Logger {
    param([string]$OutFile, [int]$Seconds, [string]$Cmd = "")
    Remove-Item -LiteralPath $OutFile -Force -ErrorAction SilentlyContinue
    $args = @($script:Com9Logger, $OutFile, "$Seconds")
    $args += @("--port", (Get-DebugPort))
    if ($Cmd) { $args += @("--cmd", $Cmd) }
    Start-Process -FilePath $script:Python -ArgumentList $args `
        -WindowStyle Hidden -PassThru
}

function Get-DebugPort {
    # 调试串口解析优先级：环境变量 D00_DEBUG_PORT > 已配置端口在线 > 首个可用串口 > COM5。
    # 换 USB 口后 Windows 会重新分配 COM 号，自动探测可避免脚本因端口漂移而失效。
    if ($env:D00_DEBUG_PORT) { return $env:D00_DEBUG_PORT }
    try {
        $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    } catch {
        $ports = @()
    }
    if ($ports -contains $script:DebugPort) { return $script:DebugPort }
    if ($ports.Count -gt 0) { return $ports[0] }
    return $script:DebugPort
}

function Stop-Logger {
    param($Process)
    try {
        if ($Process -and -not $Process.HasExited) { $Process.Kill() }
    } catch {}
}

function Test-BootLog {
    param([string]$Text)
    # Historical crash-recovery blocks ("[CRASH] Previous crash recovered ..."
    # plus indented continuation lines) are info only, NOT failures. Strip all
    # [CRASH] lines before the bad-marker scan; active crashes are detected
    # separately below on the original text.
    $cleanText = @(($Text -split "`r?`n") | Where-Object { $_ -notmatch "\[CRASH\]" }) -join "`n"
    $missing = @()
    foreach ($pat in $script:VerifyExpect) {
        if ($cleanText -notmatch [regex]::Escape($pat)) { $missing += $pat }
    }
    $bad = @()
    foreach ($pat in $script:VerifyFail) {
        if ($cleanText -match [regex]::Escape($pat)) { $bad += $pat }
    }
    # Crash records: "[CRASH] Previous crash recovered: #N, cause, uptime" is
    # historical info (firmware recovered and booted). Continuation lines start
    # with 2+ spaces. Only a non-recovered crash HEADER is an active failure.
    $crashLines = @(($Text -split "`r?`n") | Where-Object { $_ -match "\[CRASH\]" })
    $recovered = @($crashLines | Where-Object { $_ -match "Previous crash recovered" })
    $activeCrash = @($crashLines | Where-Object {
        ($_ -match "^\[CRASH\]\s\S") -and ($_ -notmatch "Previous crash recovered")
    })
    if ($activeCrash.Count -gt 0) { $bad += "[CRASH] active" }
    [pscustomobject]@{
        Pass      = (($missing.Count -eq 0) -and ($bad.Count -eq 0) -and ($Text.Length -gt 0))
        Missing   = $missing
        Bad       = $bad
        Recovered = $recovered
    }
}
