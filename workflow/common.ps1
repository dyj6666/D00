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
$script:OpenOCD    = "D:\GIT-SPACE\D00\tools\xpack-openocd-0.12.0-7\bin\openocd.exe"
$script:OpenOcdScripts = "D:\GIT-SPACE\D00\tools\xpack-openocd-0.12.0-7\openocd\scripts"
$script:Python     = "D:\Python\python.exe"
$script:Cmake      = "cmake"
$script:Ninja      = "ninja"
$script:DapFlash   = Join-Path $script:WorkflowDir "flash_dap.ps1"
$script:OtaTcpCli  = Join-Path $script:AppRoot "Script\ota_tcp_cli.py"

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
$script:AppSectors  = @("4","5","6","7","8","9","10")  # RUN 832KB = 扇区 4-10（方案B）

$script:DebugPort = if ($env:D00_DEBUG_PORT) { $env:D00_DEBUG_PORT } else { "COM5" }
$script:HostPort  = if ($env:D00_HOST_PORT)  { $env:D00_HOST_PORT  } else { "" }
$script:BootLog   = Join-Path $AppRoot "_auto_boot.txt"
$script:OtaLog    = Join-Path $AppRoot "_auto_ota.txt"

# Version written next to the APP validity magic (0x080DFFFC, scheme B).
# Defaults live in config/version.json (single source of truth) and
# override these fallback values; keep both consistent before release.
$script:OtaVersion = 213
$script:OtaBuildNo = 9156
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
$script:VerifyFail   = @("HardFault", "UsageFault", "assert", "FATAL",
                         "SELF-TEST FAILED", "SPOT CHECK FAIL", "LCD] WritePixels OOB")

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
    # 注意：不能用 -match "0 Error(s)" 判定成功——"30 Error(s)" 含子串
    # "0 Error(s)" 会误判 OK（2026-08-19 实测：RAM 溢出 30 Error 被判成功，
    # 导致 2 轮 OTA 推送旧固件）。必须解析数字比较。
    $errors = -1
    if ($txt -match "(\d+)\s+Error\(s\)") { $errors = [int]$Matches[1] }
    $ok = ($errors -eq 0)
    $warnings = 0
    if ($txt -match "(\d+)\s+Warning\(s\)") { $warnings = [int]$Matches[1] }
    [pscustomobject]@{ Ok = $ok; Warnings = $warnings; Errors = $errors; Text = $txt }
}

function Start-Com9Logger {
    param([string]$OutFile, [int]$Seconds, [string]$Cmd = "", [double]$CmdDelay = 0.0)
    Remove-Item -LiteralPath $OutFile -Force -ErrorAction SilentlyContinue
    $args = @($script:Com9Logger, $OutFile, "$Seconds")
    $args += @("--port", (Get-DebugPort))
    if ($Cmd) {
        # Start-Process 数组参数对含空格元素不加引号，命令本身需显式包裹
        $args += @("--cmd", ('"' + $Cmd + '"'))
        if ($CmdDelay -gt 0) { $args += @("--cmd-delay", "$CmdDelay") }
    }
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

function Get-AllSerialPorts {
    # Returns all available serial ports (COMx names). Never throws.
    try {
        return @([System.IO.Ports.SerialPort]::GetPortNames())
    } catch {
        return @()
    }
}

function Get-HostPort {
    # HOSTLINK (USART1) / YMODEM port resolver.
    # Priority: env D00_HOST_PORT > configured > first port that is NOT the
    # debug port (covers COM-number drift after replug). Falls back to "".
    if ($env:D00_HOST_PORT) { return $env:D00_HOST_PORT }
    $ports = Get-AllSerialPorts
    if ($script:HostPort -and ($ports -contains $script:HostPort)) {
        return $script:HostPort
    }
    $dbg = Get-DebugPort
    foreach ($p in $ports) {
        if ($p -ne $dbg) { return $p }
    }
    if ($script:HostPort) { return $script:HostPort }
    return ""
}

function Test-DapOnline {
    # Probe CMSIS-DAP via OpenOCD (halt + read DPIDR). Returns $true when a
    # DAP probe is connected and the target answers on SWD.
    if (-not (Test-Path -LiteralPath $script:OpenOCD)) { return $false }
    $work = Join-Path $env:TEMP ("dap_probe_" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $work | Out-Null
    $log = Join-Path $work "probe.log"
    try {
        $r = Invoke-Exe -FilePath $script:OpenOCD -Arguments @(
            "-s", $script:OpenOcdScripts,
            "-f", "interface/cmsis-dap.cfg",
            "-f", "target/stm32f4x.cfg",
            "-c", "adapter speed 500",
            "-c", "init", "-c", "halt", "-c", "shutdown"
        ) -LogFile $log -TimeoutSec 25
        # OpenOCD logs to stderr; Inspect the captured log file.
        $logText = ""
        if (Test-Path -LiteralPath $log) { $logText = Get-Content -LiteralPath $log -Raw }
        return ($r.ExitCode -eq 0) -and ($logText -match "SWD DPIDR")
    } catch {
        return $false
    }
}

function Invoke-OpenOcdReset {
    # Reset the target via CMSIS-DAP (OpenOCD). Works with DAP probes only.
    # Prefer this over STM32CubeProgrammer (which does not support CMSIS-DAP).
    param([switch]$Halt)
    $args = @(
        "-s", $script:OpenOcdScripts,
        "-f", "interface/cmsis-dap.cfg",
        "-f", "target/stm32f4x.cfg",
        "-c", "adapter speed 500",
        "-c", "init"
    )
    if ($Halt) { $args += @("-c", "halt", "-c", "reg pc") }
    else       { $args += @("-c", "reset run") }
    $args += @("-c", "shutdown")
    $r = Invoke-Exe -FilePath $script:OpenOCD -Arguments $args -TimeoutSec 60
    if ($r.ExitCode -ne 0) {
        throw "OpenOCD reset failed (DAP offline?)"
    }
    if ($Halt) { return $r.Stdout }
}

function Reset-Target {
    # Unified reset: DAP (OpenOCD) -> ST-Link (STM32CubeProgrammer) -> serial.
    # Returns the method actually used ("dap"/"stlink"/"serial"/"none").
    param([switch]$SerialReset, [switch]$NoReset)
    if ($NoReset) { return "none" }
    if ($SerialReset) {
        $dbg = Get-DebugPort
        $logger = Start-Com9Logger -OutFile (Join-Path $script:AppRoot "_auto_serialreset.txt") `
            -Seconds 6 -Cmd "reset"
        Start-Sleep -Seconds 2
        Stop-Logger $logger
        return "serial"
    }
    if (Test-DapOnline) {
        Invoke-OpenOcdReset
        return "dap"
    }
    # ST-Link fallback
    $r = Invoke-Exe -FilePath $script:Programmer -Arguments @("-c", "port=SWD", "-rst") -TimeoutSec 60
    if ($r.ExitCode -eq 0) { return "stlink" }
    throw "No working reset path (DAP offline and ST-Link absent). Use -SerialReset."
}

function Test-ProjectNoBom {
    # Project/config files (XML scatter/linker) must be BOM-free, otherwise
    # Keil silently rejects the project. Returns list of offending files.
    param([string[]]$Files)
    $bad = @()
    foreach ($f in $Files) {
        if (-not (Test-Path -LiteralPath $f)) { continue }
        $bytes = [System.IO.File]::ReadAllBytes($f)
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
            $bad += $f
        }
    }
    return $bad
}

function Test-StaleObjects {
    # Detects Keil incremental-build staleness: an object newer source file
    # (global config headers force recompile of dependents, but Keil -b does
    # not always track them). Returns $true when a full rebuild (-Clean) is
    # recommended. Compares key config headers vs every .o in MDK-ARM output.
    param([string]$ProjectDir)
    $objDir = Join-Path $ProjectDir "MDK-ARM\APP"
    if (-not (Test-Path -LiteralPath $objDir)) { return $false }
    $headers = @(
        (Join-Path $ProjectDir "Config\app_config.h"),
        (Join-Path $ProjectDir "Core\Inc\FreeRTOSConfig.h"),
        (Join-Path $ProjectDir "Middlewares\Third_Party\lvgl\lv_conf.h"),
        (Join-Path $ProjectDir "Core\Src\main.c"),
        (Join-Path $ProjectDir "MDK-ARM\APP.uvprojx")
    )
    $latestHeader = 0
    foreach ($h in $headers) {
        if (Test-Path -LiteralPath $h) {
            $t = (Get-Item -LiteralPath $h).LastWriteTimeUtc.Ticks
            if ($t -gt $latestHeader) { $latestHeader = $t }
        }
    }
    if ($latestHeader -eq 0) { return $false }
    $objs = Get-ChildItem -LiteralPath $objDir -Filter *.o -ErrorAction SilentlyContinue
    $stale = @($objs | Where-Object { $_.LastWriteTimeUtc.Ticks -lt $latestHeader })
    return ($stale.Count -gt 0)
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
