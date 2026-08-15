param(
    [ValidateSet("APP","BOOT","ALL")] [string]$Project = "ALL",
    [ValidateSet("Keil","GCC")] [string]$Toolchain = "Keil",
    [switch]$Clean
)
. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $script:LogDir | Out-Null

$report = New-Report
$report.git = Get-GitHead
$script:buildFailed = $false

function Add-Artifact {
    param([string]$Tag, [string]$Path)
    if (Test-Path -LiteralPath $Path) {
        $i = Get-Item -LiteralPath $Path
        $report.stages[$Tag + "_artifact"] = [pscustomobject]@{
            path  = $Path
            bytes = $i.Length
            time  = $i.LastWriteTime.ToString("o")
        }
    }
}

function Build-Keil {
    param([string]$ProjectPath, [string]$Tag)
    $log = Join-Path $script:LogDir ("build_" + $Tag + "_keil.log")
    Write-Step ("Keil build " + $Tag)
    if ($Clean) {
        $r = Invoke-UV4 -Project $ProjectPath -LogFile $log -Rebuild
    } else {
        $r = Invoke-UV4 -Project $ProjectPath -LogFile $log
    }
    $t = Test-KeilLog $log
    $ok = $t.Ok
    $report.stages[$Tag] = $(if ($ok) { "OK" } else { "FAIL" })
    $report.stages[$Tag + "_warnings"] = $t.Warnings
    $report.stages[$Tag + "_log"] = $log
    if ($ok) {
        Write-Host ("BUILD OK: " + $Tag + " (warnings=" + $t.Warnings + ")")
    } else {
        Write-Host ("BUILD FAILED: " + $Tag)
        Get-Content -LiteralPath $log -Tail 30 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        $script:buildFailed = $true
    }
}

function Build-Gcc-App {
    $log = Join-Path $script:LogDir "build_APP_gcc.log"
    Write-Step "GCC build APP (cmake + ninja)"
    $args = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $script:CmakeBuildPs1)
    if ($Clean) { $args += "-Clean" }
    $r = Invoke-Exe -FilePath "powershell.exe" -Arguments $args -LogFile $log -TimeoutSec 1200
    $ok = ($r.ExitCode -eq 0) -and (Test-Path -LiteralPath $script:AppGccBin)
    $report.stages["APP_gcc"] = $(if ($ok) { "OK" } else { "FAIL" })
    $report.stages["APP_gcc_log"] = $log
    if ($ok) {
        Write-Host "BUILD OK: APP (GCC)"
        Add-Artifact "APP_gcc" $script:AppGccBin
    } else {
        Write-Host "BUILD FAILED: APP (GCC)"
        Get-Content -LiteralPath $log -Tail 30 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        $script:buildFailed = $true
    }
}

function Build-Gcc-Boot {
    $log = Join-Path $script:LogDir "build_BOOT_gcc.log"
    Write-Step "GCC build BOOT (cmake + ninja)"
    $build = Join-Path $script:BootRoot "build-fw"
    if ($Clean) { Remove-Item -LiteralPath $build -Recurse -Force -ErrorAction SilentlyContinue }
    $tc = Join-Path $script:BootRoot "cmake\toolchain-stm32f4.cmake"
    $cfg = Invoke-Exe -FilePath $script:Cmake -Arguments @("-S", $script:BootRoot, "-B", $build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=$tc") -LogFile $log -TimeoutSec 600
    $mk = $null
    if ($cfg.ExitCode -eq 0) {
        $mk = Invoke-Exe -FilePath $script:Cmake -Arguments @("--build", $build) -LogFile $log -TimeoutSec 1200
    }
    $ok = ($cfg.ExitCode -eq 0) -and ($null -ne $mk) -and ($mk.ExitCode -eq 0) -and (Test-Path -LiteralPath $script:BootGccBin)
    $report.stages["BOOT_gcc"] = $(if ($ok) { "OK" } else { "FAIL" })
    $report.stages["BOOT_gcc_log"] = $log
    if ($ok) {
        Write-Host "BUILD OK: BOOT (GCC)"
        Add-Artifact "BOOT_gcc" $script:BootGccBin
    } else {
        Write-Host "BUILD FAILED: BOOT (GCC)"
        Get-Content -LiteralPath $log -Tail 30 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        $script:buildFailed = $true
    }
}

if ($Project -in @("APP","ALL")) {
    if ($Toolchain -eq "Keil") {
        Build-Keil $script:AppUvprojx "APP"
        Add-Artifact "APP" $script:AppBin
    } else {
        Build-Gcc-App
    }
}
if ($Project -in @("BOOT","ALL")) {
    if ($Toolchain -eq "Keil") {
        Build-Keil $script:BootUvprojx "BOOT"
        Add-Artifact "BOOT" $script:BootBin
    } else {
        Build-Gcc-Boot
    }
}

$report.generated = Get-Date -Format "o"
Save-Report $report
if ($script:buildFailed) { Write-Host "BUILD STAGE FAILED"; exit 1 }
Write-Host "BUILD STAGE PASSED"
exit 0
