# 固件业务代码交叉编译语法冒烟测试（arm-none-eabi-gcc -fsyntax-only）
# 用途：在没有 Keil 的 CI/本机环境快速发现语法与类型错误。
$ErrorActionPreference = 'Stop'

$arm = 'D:\STM32CUBECLT\STM32CubeCLT_1.18.0\GNU-tools-for-STM32\bin\arm-none-eabi-gcc.exe'
if (-not (Test-Path $arm)) {
    $cmd = Get-Command arm-none-eabi-gcc -ErrorAction SilentlyContinue
    if ($cmd) { $arm = $cmd.Source } else { throw 'arm-none-eabi-gcc not found' }
}

$incs = @(
    'Script/gcc_port',
    'Core/Inc',
    'BSP',
    'SystemServices',
    'Application',
    'Config',
    'Drivers',
    'Drivers/CMSIS/Include',
    'Drivers/CMSIS/Device/ST/STM32F4xx/Include',
    'Drivers/STM32F4xx_HAL_Driver/Inc',
    'Drivers/STM32F4xx_HAL_Driver/Inc/Legacy',
    'Middlewares/Third_Party/FreeRTOS/Source/include',
    'Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2'
)

$srcs = @(
    'BSP/bsp_system.c',
    'BSP/bsp_uart.c',
    'BSP/bsp_watchdog.c',
    'BSP/bsp_rtc.c',
    'BSP/bsp_gpio.c',
    'SystemServices/crc16.c',
    'SystemServices/protocol.c',
    'SystemServices/var_list.c',
    'SystemServices/data_link.c',
    'SystemServices/event_bus.c',
    'SystemServices/var_manager.c',
    'SystemServices/watchdog.c',
    'SystemServices/sysmon.c',
    'SystemServices/logger.c',
    'SystemServices/shell.c',
    'SystemServices/la_buffer.c',
    'SystemServices/la_trigger.c',
    'SystemServices/la_sample.c',
    'SystemServices/module.c',
    'Application/key_app.c',
    'Application/led_app.c',
    'Application/ota_agent.c',
    'Application/data_agent.c',
    'Core/Src/main.c',
    'Core/Src/freertos.c',
    'Core/Src/tim.c',
    'Core/Src/dma.c',
    'Core/Src/usart.c',
    'Core/Src/stm32f4xx_it.c'
)

$gcc_args = @(
    '-mcpu=cortex-m4', '-mthumb', '-std=c99',
    '-Wall', '-Wextra', '-Wno-unused-parameter',
    '-fsyntax-only',
    '-DSTM32F407xx', '-DUSE_HAL_DRIVER'
)
foreach ($i in $incs) { $gcc_args += "-I$i" }

$failed = 0
foreach ($s in $srcs) {
    Write-Host "==> $s"
    & $arm @gcc_args $s
    if ($LASTEXITCODE -ne 0) {
        $failed = 1
    }
}

if ($failed -eq 0) {
    Write-Host 'FIRMWARE SYNTAX OK'
    exit 0
}
exit 1
