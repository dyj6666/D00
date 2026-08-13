# ================================================================
# flash_dap - robust CMSIS-DAP flasher (fixes DAP freeze/red LED)
#
# Background: KEIL DAP freezes (HID timeout 0x3E5, red LED) after
# ~20s of sustained writes at 2MHz SWD. Measured: 500kHz + small
# chunks fully avoids it. Also, the target IWDG keeps running while
# halted (~4s timeout), so long writes get reset-interrupted.
#
# Four defenses:
#   1) SWD clock fixed at 500kHz (243KB write+verify proven stable);
#   2) Chunked writes (default 48KB), IWDG refreshed (KR=0xAAAA)
#      between chunks so the target never watchdog-resets mid-flash;
#   3) Erase once, write via flash write_bank (no auto-erase), then
#      a separate verify_image pass over the whole image;
#   4) HID timeout auto-retry (3s cooldown), USB re-enum recovery on
#      repeated failures, clear manual-replug hint as last resort.
#
# Usage:
#   powershell -File workflow\flash_dap.ps1 -File <bin> -Addr <hex>
#   powershell -File workflow\flash_dap.ps1 -File BOOT.bin -Addr 0x08000000
#   powershell -File workflow\flash_dap.ps1 -File pkg.bin -Addr 0x080A0000 -ChunkKB 32
# Options: -ClockKHz (default 500), -ChunkKB (default 48),
#          -MaxRetry (default 3), -NoReset (keep target running after).
# ================================================================

param(
    [Parameter(Mandatory = $true)][string]$File,
    [Parameter(Mandatory = $true)][long]$Addr,
    [int]$ClockKHz = 500,
    [int]$ChunkKB = 48,
    [int]$MaxRetry = 3,
    [switch]$NoReset
)

# OpenOCD logs to stderr by design; keep EAP=Continue and check
# results explicitly (Stop would turn normal log noise into errors).
$ErrorActionPreference = "Continue"
$OpenOCD = "D:\GIT-SPACE\D00\tools\xpack-openocd-0.12.0-7\bin\openocd.exe"
$Scripts = "D:\GIT-SPACE\D00\tools\xpack-openocd-0.12.0-7\openocd\scripts"
$FlashBase = 0x08000000
$IwdgKr = "0x40003000"
$IwdgPr = "0x40003004"
$IwdgRlr = "0x40003008"

if (-not (Test-Path -LiteralPath $File)) { throw "File not found: $File" }
$Size = (Get-Item -LiteralPath $File).Length
$Len = [int64]([math]::Ceiling($Size / 1024.0) * 1024)
Write-Host ("[DAP] {0} ({1} B) -> 0x{2:X8}  clock={3}kHz chunk={4}KB" -f `
            $File, $Size, $Addr, $ClockKHz, $ChunkKB)

$Work = Join-Path $env:TEMP ("dap_flash_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $Work | Out-Null

function Split-Chunks {
    $bytes = [System.IO.File]::ReadAllBytes($File)
    $chunkBytes = $ChunkKB * 1024
    $index = 0
    for ($off = 0; $off -lt $bytes.Length; $off += $chunkBytes) {
        $n = [Math]::Min($chunkBytes, $bytes.Length - $off)
        $part = New-Object byte[] $n
        [Array]::Copy($bytes, $off, $part, 0, $n)
        $path = Join-Path $Work ("chunk_{0:D2}.bin" -f $index)
        [System.IO.File]::WriteAllBytes($path, $part)
        $index++
    }
    return $index
}

function Get-EraseRange([long]$Addr, [long]$Size) {
    # F407 1MB sector map: s0-s3 = 4x16KB @ 0x08000000; s4 = 64KB @ 0x08010000;
    # s5-s11 = 7x128KB @ 0x08020000. erase_address requires both ends aligned.
    $end = $Addr + $Size - 1
    if ($Addr -lt 0x08010000) {
        $start = 0x08000000 + [math]::Floor(($Addr - 0x08000000) / 0x4000) * 0x4000
    } elseif ($Addr -lt 0x08020000) {
        $start = 0x08010000
    } else {
        $start = 0x08020000 + [math]::Floor(($Addr - 0x08020000) / 0x20000) * 0x20000
    }
    if ($end -lt 0x08010000) {
        $endBound = 0x08000000 + [math]::Ceiling(($end + 1 - 0x08000000) / 0x4000) * 0x4000
    } elseif ($end -lt 0x08020000) {
        $endBound = 0x08020000
    } else {
        $endBound = 0x08020000 + [math]::Ceiling(($end + 1 - 0x08020000) / 0x20000) * 0x20000
    }
    return ,@($start, [int64]($endBound - $start))
}

function Hex8([long]$Value) {
    return $Value.ToString("X8")
}

function FwdPath([string]$Path) {
    # Paths inside OpenOCD -c strings go through Tcl parsing, where
    # backslashes act as escapes; normalize to forward slashes.
    return $Path.Replace('\', '/')
}

function Invoke-OpenOCD([string[]]$Cmds) {
    $argList = @("-s", $Scripts, "-f", "interface/cmsis-dap.cfg",
                 "-f", "target/stm32f4x.cfg",
                 "-c", "adapter speed $ClockKHz")
    foreach ($c in $Cmds) { $argList += @("-c", $c) }
    $log = Join-Path $Work "openocd.log"
    $out = & $OpenOCD $argList 2>&1 | Out-String
    Set-Content -LiteralPath $log -Value $out
    return $out
}

function Test-HidError([string]$out) {
    return ($out -match "hid_write|error writing data|CMD_INFO failed")
}

function Test-Verified([string]$out) {
    # verify_image prints "verified <n> bytes"; program prints "Verified OK".
    return ($out -match "verified \d+ bytes|Verified OK")
}

function Reset-UsbProbe {
    # Best-effort USB re-enumeration (needs admin); non-fatal
    try {
        $dev = Get-PnpDevice -ErrorAction SilentlyContinue |
               Where-Object { $_.FriendlyName -match "CMSIS-DAP|DAP" -or
                              $_.InstanceId -match "VID_0D28" }
        if ($dev) {
            $dev | Disable-PnpDevice -Confirm:$false -ErrorAction Stop
            Start-Sleep -Seconds 2
            $dev | Enable-PnpDevice -Confirm:$false -ErrorAction Stop
            Write-Host "[DAP] USB re-enumerated"
            return
        }
    } catch {
        Write-Host "[DAP] USB re-enum failed (admin needed); replug manually"
    }
}

try {
    $nChunks = Split-Chunks

    for ($attempt = 1; $attempt -le $MaxRetry; $attempt++) {
        Write-Host "[DAP] attempt $attempt/$MaxRetry ..."
        $erase = Get-EraseRange $Addr $Size

        # ---- single session: reset-halt + IWDG extend + erase + write + verify ----
        # reset halt puts the target into a known state (clears flash state
        # dirt from previous interrupted ops). The IWDG prescaler is
        # temporarily raised to 256 (~22s window) via the debugger so the
        # whole flash (~13s) never watchdog-resets; after reset run the
        # bootloader re-inits IWDG back to its normal ~4.1s config.
        $cmds = @(
            "init",
            "reset halt",
            "mww $IwdgKr 0x5555",
            "mww $IwdgPr 0x6",
            "mww $IwdgRlr 0xFFF",
            "mww $IwdgKr 0xAAAA",
            ("flash erase_address 0x" + (Hex8 $erase[0]) + " 0x" + (Hex8 $erase[1])),
            ("flash write_bank 0 " + (FwdPath (Join-Path $Work 'chunk_00.bin')) + " 0x" + (Hex8 ($Addr - $FlashBase)))
        )
        for ($i = 1; $i -lt $nChunks; $i++) {
            $cmds += "mww $IwdgKr 0xAAAA"
            $off = $Addr + $i * $ChunkKB * 1024 - $FlashBase
            $cmds += ("flash write_bank 0 " + (FwdPath (Join-Path $Work ('chunk_{0:D2}.bin' -f $i))) + " 0x" + (Hex8 $off))
        }
        $cmds += "mww $IwdgKr 0xAAAA"
        $cmds += ("verify_image " + (FwdPath $File) + " 0x" + (Hex8 $Addr))
        if ($NoReset) {
            $cmds += "shutdown"
        } else {
            $cmds += "reset run"
            $cmds += "shutdown"
        }
        $out = Invoke-OpenOCD $cmds

        if (Test-HidError $out) {
            Write-Host "[DAP] HID timeout (attempt $attempt), cooldown 3s..."
            Copy-Item -LiteralPath (Join-Path $Work 'openocd.log') -Destination 'D:\GIT-SPACE\D00\_dap_fail.log' -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 3
            if ($attempt -ge 2) { [void](Reset-UsbProbe) }
            continue
        }
        if (-not (Test-Verified $out)) {
            Write-Host "[DAP] verify failed (attempt $attempt)"
            Copy-Item -LiteralPath (Join-Path $Work 'openocd.log') -Destination 'D:\GIT-SPACE\D00\_dap_fail.log' -Force -ErrorAction SilentlyContinue
            if ($attempt -ge 2) { [void](Reset-UsbProbe) }
            continue
        }

        Write-Host "[DAP] flash + verify OK"
        return 0
    }

    Write-Host "[DAP] FAILED after $MaxRetry attempts (HID timeout/verify)"
    Write-Host "[DAP] Replug the DAP (red LED = frozen) and retry"
    return 1
}
finally {
    if (Test-Path -LiteralPath $Work) {
        Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
    }
}
