param(
    [ValidateSet("selfcheck","build","quick","flash","verify","ota","hosttest","full")] [string]$Mode = "full",
    [switch]$SkipBuild,
    [switch]$SkipFlash,
    [switch]$SkipVerify,
    [switch]$SkipOta,
    [switch]$SkipHostTest,
    [switch]$SkipSelfCheck,
    [switch]$IncludeOta,
    [int]$Version = 0,
    [int]$BuildNo = 0
)
. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $script:LogDir | Out-Null

$doBuild = (-not $SkipBuild)
$doFlash = (-not $SkipFlash)
$doVerify = (-not $SkipVerify)
$doOta = $IncludeOta
$doHost = (-not $SkipHostTest)
$doSelf = (-not $SkipSelfCheck)

switch ($Mode) {
    # 每个模式只关闭"不属于本模式"的阶段；-Skip* 开关始终生效（先于 switch 计算）。
    "selfcheck" { $doBuild = $false; $doFlash = $false; $doVerify = $false; $doOta = $false; $doHost = $false }
    "build"     { $doSelf = $false; $doFlash = $false; $doVerify = $false; $doOta = $false; $doHost = $false }
    "quick"     { $doSelf = $false; $doFlash = $false; $doVerify = $false; $doOta = $false }
    "flash"     { $doSelf = $false; $doBuild = $false; $doOta = $false; $doHost = $false }
    "verify"    { $doSelf = $false; $doBuild = $false; $doFlash = $false; $doOta = $false; $doHost = $false }
    "ota"       { $doSelf = $false; $doBuild = $false; $doFlash = $false; $doVerify = $false; $doHost = $false }
    "hosttest"  { $doSelf = $false; $doBuild = $false; $doFlash = $false; $doVerify = $false; $doOta = $false }
    "full"      { }
}

$report = New-Report
$report.git = Get-GitHead
$failed = New-Object System.Collections.ArrayList

function Run-Stage {
    param([string]$Name, [string]$ScriptName, [string[]]$StageArgs = @())
    Write-Host ""
    Write-Host ("========== STAGE: " + $Name + " ==========")
    $t0 = Get-Date
    $scriptFile = Join-Path $PSScriptRoot $ScriptName
    & $scriptFile @StageArgs
    $code = $LASTEXITCODE
    $sec = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    $report.stages[$Name] = $(if ($code -eq 0) { "OK" } else { "FAIL(" + $code + ")" })
    $report.stages[$Name + "_sec"] = $sec
    Write-Host ("[" + $Name + "] " + $sec + "s")
    return $code
}

if ($doSelf) {
    $c = Run-Stage "selfcheck" "self_check.ps1"
    if ($c -eq 1) { [void]$failed.Add("selfcheck") }
}
if ($doBuild) {
    $c = Run-Stage "build" "auto_build.ps1" @("-Project", "ALL")
    if ($c -ne 0) { [void]$failed.Add("build") }
}
if ($doFlash) {
    $c = Run-Stage "flash" "auto_flash.ps1"
    if ($c -ne 0) { [void]$failed.Add("flash") }
}
if ($doVerify) {
    $c = Run-Stage "verify" "auto_verify.ps1"
    if ($c -ne 0) { [void]$failed.Add("verify") }
}
if ($doOta) {
    $args = @()
    if ($Version -gt 0) { $args += @("-Version", "$Version") }
    if ($BuildNo -gt 0) { $args += @("-BuildNo", "$BuildNo") }
    $c = Run-Stage "ota" "auto_ota.ps1" $args
    if ($c -ne 0) { [void]$failed.Add("ota") }
}
if ($doHost) {
    $c = Run-Stage "hosttest" "auto_hosttest.ps1"
    if ($c -ne 0) { [void]$failed.Add("hosttest") }
}

$report.stages["mode"] = $Mode
$report.stages["summary"] = $(if ($failed.Count -eq 0) { "PASS" } else { "FAIL: " + ($failed -join ", ") })
$report.artifacts = [ordered]@{}
foreach ($a in @(
    @{ tag = "boot"; path = $script:BootBin },
    @{ tag = "app";  path = $script:AppBin }
)) {
    if (Test-Path -LiteralPath $a.path) {
        $i = Get-Item -LiteralPath $a.path
        $report.artifacts[$a.tag] = [ordered]@{
            path   = $a.path
            bytes  = $i.Length
            sha256 = Get-FileSha256 $a.path
        }
    }
}
$report.generated = Get-Date -Format "o"
Save-Report $report

Write-Host ""
Write-Host ("========== PIPELINE " + $(if ($failed.Count -eq 0) { "PASSED" } else { "FAILED: " + ($failed -join ", ") }) + " ==========")
Write-Host ("Report: " + $script:ReportPath)
exit $(if ($failed.Count -eq 0) { 0 } else { 1 })
