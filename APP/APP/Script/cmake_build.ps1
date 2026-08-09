param(
    [switch]$Clean
)
# D00 APP CMake 快速增量构建（GCC + Ninja）
# 用法: powershell -File Script\cmake_build.ps1        # 增量构建
#       powershell -File Script\cmake_build.ps1 -Clean # 全量重建
$ErrorActionPreference = "Stop"
$Root  = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root "build-cmake"

if ($Clean) {
    Remove-Item -LiteralPath $Build -Recurse -Force -ErrorAction SilentlyContinue
}

cmake -S $Root -B $Build -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_TOOLCHAIN_FILE="$Root\cmake\arm-none-eabi-toolchain.cmake"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $Build
exit $LASTEXITCODE
