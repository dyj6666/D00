param(
    [switch]$SkipBoot,
    [switch]$SkipApp,
    [switch]$Robustness
)
. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $script:LogDir | Out-Null

$report = New-Report
$report.git = Get-GitHead
$fail = $false

if (-not $SkipBoot) {
    $log = Join-Path $script:LogDir "hosttest_boot.log"
    Write-Step "BOOT host unit tests (cmake + ctest)"
    $build = Join-Path $script:BootRoot "build-host"
    $cfg = Invoke-Exe -FilePath $script:Cmake -Arguments @("-S", $script:BootRoot, "-B", $build, "-G", "Ninja") -LogFile $log -TimeoutSec 600
    $mk = $null
    $ct = $null
    if ($cfg.ExitCode -eq 0) {
        $mk = Invoke-Exe -FilePath $script:Cmake -Arguments @("--build", $build, "--target", "host_tests") -LogFile $log -TimeoutSec 1200
        if ($mk.ExitCode -eq 0) {
            $ct = Invoke-Exe -FilePath "ctest" -Arguments @("--test-dir", $build, "--output-on-failure") -LogFile $log -TimeoutSec 600
        }
    }
    $ok = ($null -ne $ct) -and ($ct.ExitCode -eq 0)
    $report.stages["hosttest_boot"] = $(if ($ok) { "OK" } else { "FAIL" })
    $report.stages["hosttest_boot_log"] = $log
    if ($ok) {
        Write-Host "BOOT HOST TESTS OK"
    } else {
        $fail = $true
        Write-Host "BOOT HOST TESTS FAILED"
        Get-Content -LiteralPath $log -Tail 40 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
    }
}

if (-not $SkipApp) {
    $exe = Join-Path $script:AppRoot "tests\test_protocol.exe"
    if (Test-Path -LiteralPath $exe) {
        Write-Step "APP protocol test (prebuilt host binary)"
        $log = Join-Path $script:LogDir "hosttest_app.log"
        $r = Invoke-Exe -FilePath $exe -LogFile $log -TimeoutSec 300
        $ok = $r.ExitCode -eq 0
        $report.stages["hosttest_app"] = $(if ($ok) { "OK" } else { "FAIL" })
        $report.stages["hosttest_app_log"] = $log
        if ($ok) {
            Write-Host "APP HOST TEST OK"
        } else {
            $fail = $true
            Write-Host "APP HOST TEST FAILED"
            Get-Content -LiteralPath $log -Tail 40 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        }
    } else {
        Write-Step "APP prebuilt host test not found, skipped"
        $report.stages["hosttest_app"] = "SKIP (test_protocol.exe not built)"
    }
}

if ($Robustness) {
    $exe = Join-Path $script:HostRoot "LogicAnalyzer\tests\test_robustness.py"
    if (Test-Path -LiteralPath $exe) {
        Write-Step "LogicAnalyzer hardware robustness (COM9/COM13, 需固件 signal_gen + 物理接线)"
        $log = Join-Path $script:LogDir "hosttest_robustness.log"
        $r = Invoke-Exe -FilePath $script:Python -Arguments @($exe) -LogFile $log -TimeoutSec 1800
        $ok = $r.ExitCode -eq 0
        $report.stages["hosttest_robustness"] = $(if ($ok) { "OK" } else { "FAIL" })
        $report.stages["hosttest_robustness_log"] = $log
        if ($ok) {
            Write-Host "LA ROBUSTNESS OK"
        } else {
            $fail = $true
            Write-Host "LA ROBUSTNESS FAILED"
            Get-Content -LiteralPath $log -Tail 40 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        }
    } else {
        Write-Step "LA robustness test not found, skipped"
        $report.stages["hosttest_robustness"] = "SKIP (test_robustness.py not found)"
    }
}

$report.generated = Get-Date -Format "o"
Save-Report $report
if ($fail) { Write-Host "HOSTTEST STAGE FAILED"; exit 1 }
Write-Host "HOSTTEST STAGE PASSED"
exit 0
