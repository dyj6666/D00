# ============================================================
# fix_after_mx.ps1 - CubeMX regeneration reconciliation script
# Run AFTER regenerating code from CubeMX. Re-applies the
# "out-of-model" configuration CubeMX would otherwise wipe:
#   1. err_mgr assembly fault handlers (remove duplicate C versions)
#   2. LA sampling EXTI pins (PG6/PG7/PG8/PG15)
#   3. Signal-generator DMA2_Stream6 ISR (USART6 TX)
#   4. MX_I2C1_Init() in main.c (MPU6050)
#   5. FreeRTOSConfig.h: tick hook / heap / ERR_HandleAssert proto
#   6. HAL_ETH_MspInit in stm32f4xx_hal_msp.c
#   7. la_sample.c includes dma.h
#   8. pinout.h debug UART IRQ is USART3
# Idempotent. Prints [OK]/[FIXED]/[SKIP].
# ============================================================
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$encUtf8 = New-Object System.Text.UTF8Encoding($false)
$encGbk = [System.Text.Encoding]::GetEncoding(936)

function Read-Text([string]$path, $enc) {
  if (-not (Test-Path $path)) { return $null }
  return [IO.File]::ReadAllText($path, $enc)
}
function Write-Text([string]$path, [string]$text, $enc) {
  [IO.File]::WriteAllText($path, $text, $enc)
}

# ---- 1. it.c: remove CubeMX default fault handlers ----
$itPath = Join-Path $root 'Core\Src\stm32f4xx_it.c'
$it = Read-Text $itPath $encUtf8
if ($it -match '__asm void NMI_Handler' -and $it -match '__weak void NMI_Handler') {
  $p = '(?s)/\*\*\r?\n  \* @brief This function handles Non maskable interrupt\..*?(?=/\*\*\r?\n  \* @brief This function handles Debug monitor\.)'
  $it = [regex]::Replace($it, $p, '')
  Write-Text $itPath $it $encUtf8
  Write-Host '[FIXED] stm32f4xx_it.c: removed 5 CubeMX default fault handlers'
} else {
  Write-Host '[OK]    stm32f4xx_it.c: fault handlers clean'
}

# ---- 2. it.c: DMA2_Stream6 ISR (signal generator) ----
if ($it -notmatch 'void DMA2_Stream6_IRQHandler') {
  $s = "`r`n`r`n" + '/**' + "`r`n" + '  * @brief DMA2 stream6: signal generator USART6 TX DMA.' + "`r`n" + '  */' + "`r`n" + 'void DMA2_Stream6_IRQHandler(void)' + "`r`n" + '{' + "`r`n" + '  SG_Uart_DMA_IRQHandler();' + "`r`n" + '}' + "`r`n"
  $it = $it -replace '(/\* USER CODE BEGIN 1 \*/)', ("`$1" + $s)
  Write-Text $itPath $it $encUtf8
  Write-Host '[FIXED] stm32f4xx_it.c: re-added DMA2_Stream6_IRQHandler'
} else {
  Write-Host '[OK]    stm32f4xx_it.c: DMA2_Stream6_IRQHandler present'
}

# ---- 3. it.c: LA EXTI pins (EXTI9_5 = PG6/7/8, EXTI15_10 = PG15) ----
$need95 = ($it -match 'void EXTI9_5_IRQHandler' -and
          ($it -notmatch 'HAL_GPIO_EXTI_IRQHandler\(GPIO_PIN_6\)' -or
           $it -notmatch 'HAL_GPIO_EXTI_IRQHandler\(GPIO_PIN_7\)' -or
           $it -notmatch 'HAL_GPIO_EXTI_IRQHandler\(GPIO_PIN_8\)'))
if ($need95) {
  $b = "`r`n" + '{' + "`r`n" + '  /* USER CODE BEGIN EXTI9_5_IRQn 0 */' + "`r`n" + "`r`n" + '  /* USER CODE END EXTI9_5_IRQn 0 */' + "`r`n" + '  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_6);   /* LA CH0 = PG6 */' + "`r`n" + '  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_7);   /* LA CH1 = PG7 */' + "`r`n" + '  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_8);   /* LA CH2 = PG8 */' + "`r`n" + '  /* USER CODE BEGIN EXTI9_5_IRQn 1 */' + "`r`n" + "`r`n" + '  /* USER CODE END EXTI9_5_IRQn 1 */' + "`r`n" + '}' + "`r`n"
  $it = [regex]::Replace($it, '(?s)void EXTI9_5_IRQHandler\(void\)\s*\{.*?\}', "void EXTI9_5_IRQHandler(void)" + $b, 1)
  Write-Text $itPath $it $encUtf8
  Write-Host '[FIXED] stm32f4xx_it.c: EXTI9_5 -> PG6/PG7/PG8'
} else {
  Write-Host '[OK]    stm32f4xx_it.c: EXTI9_5 pins intact'
}
$need1510 = ($it -match 'void EXTI15_10_IRQHandler' -and
             $it -notmatch 'HAL_GPIO_EXTI_IRQHandler\(GPIO_PIN_15\)')
if ($need1510) {
  $b15 = "`r`n" + '{' + "`r`n" + '  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_15);  /* LA CH3 = PG15 */' + "`r`n" + '}' + "`r`n"
  $it = [regex]::Replace($it, '(?s)void EXTI15_10_IRQHandler\(void\)\s*\{.*?\}', "void EXTI15_10_IRQHandler(void)" + $b15, 1)
  Write-Text $itPath $it $encUtf8
  Write-Host '[FIXED] stm32f4xx_it.c: EXTI15_10 -> PG15'
} else {
  Write-Host '[OK]    stm32f4xx_it.c: EXTI15_10 pins intact'
}

# ---- 4. main.c: MX_I2C1_Init (MPU6050) ----
$mainPath = Join-Path $root 'Core\Src\main.c'
$main = Read-Text $mainPath $encUtf8
if ($main -notmatch 'MX_I2C1_Init') {
  $main = $main -replace '(MX_USART3_UART_Init\(\);)', ("`$1`r`n  MX_I2C1_Init();               /* MPU6050 (I2C1) */")
  Write-Text $mainPath $main $encUtf8
  Write-Host '[FIXED] main.c: re-added MX_I2C1_Init()'
} else {
  Write-Host '[OK]    main.c: MX_I2C1_Init() present'
}

# ---- 5. FreeRTOSConfig.h ----
$frtPath = Join-Path $root 'Core\Inc\FreeRTOSConfig.h'
$frt = Read-Text $frtPath $encUtf8
$changed = $false
if ($frt -match 'configUSE_TICK_HOOK\s+0') {
  $frt = $frt -replace 'configUSE_TICK_HOOK\s+0', 'configUSE_TICK_HOOK                      1'
  $changed = $true
}
if ($frt -match 'configTOTAL_HEAP_SIZE\s+\(\(size_t\)30720\)') {
  $frt = $frt -replace 'configTOTAL_HEAP_SIZE\s+\(\(size_t\)30720\)', 'configTOTAL_HEAP_SIZE                    ((size_t)35840)'
  $changed = $true
}
if ($frt -notmatch 'ERR_HandleAssert') {
  $frt = $frt -replace '(/\* USER CODE BEGIN 0 \*/)', ("`$1`r`n  extern void ERR_HandleAssert(uint32_t line);  /* unified configASSERT entry */")
  $changed = $true
}
if ($changed) {
  Write-Text $frtPath $frt $encUtf8
  Write-Host '[FIXED] FreeRTOSConfig.h: tick hook / heap / ERR_HandleAssert'
} else {
  Write-Host '[OK]    FreeRTOSConfig.h: settings intact'
}

# ---- 6. hal_msp.c: HAL_ETH_MspInit ----
$mspPath = Join-Path $root 'Core\Src\stm32f4xx_hal_msp.c'
$msp = Read-Text $mspPath $encUtf8
if ($msp -notmatch 'void HAL_ETH_MspInit') {
  $e = "`r`n`r`n" + '/**' + "`r`n" + '  * @brief ETH MSP: clock + RMII GPIO + PHY reset + ETH IRQ.' + "`r`n" + '  */' + "`r`n" + 'void HAL_ETH_MspInit(ETH_HandleTypeDef *heth)' + "`r`n" + '{' + "`r`n" + '  GPIO_InitTypeDef GPIO_InitStruct = {0};' + "`r`n" + "`r`n" + '  if (heth->Instance == ETH) {' + "`r`n" + '    __HAL_RCC_ETH_CLK_ENABLE();' + "`r`n" + '    __HAL_RCC_GPIOA_CLK_ENABLE();' + "`r`n" + '    __HAL_RCC_GPIOB_CLK_ENABLE();' + "`r`n" + '    __HAL_RCC_GPIOC_CLK_ENABLE();' + "`r`n" + '    GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;' + "`r`n" + '    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;' + "`r`n" + '    GPIO_InitStruct.Pull = GPIO_NOPULL;' + "`r`n" + '    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;' + "`r`n" + '    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;' + "`r`n" + '    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);' + "`r`n" + '    GPIO_InitStruct.Pin = GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13;' + "`r`n" + '    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);' + "`r`n" + '    GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;' + "`r`n" + '    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);' + "`r`n" + '    HAL_GPIO_WritePin(PHY_RESET_GPIO_Port, PHY_RESET_Pin, GPIO_PIN_RESET);' + "`r`n" + '    HAL_Delay(10);' + "`r`n" + '    HAL_GPIO_WritePin(PHY_RESET_GPIO_Port, PHY_RESET_Pin, GPIO_PIN_SET);' + "`r`n" + '    HAL_NVIC_SetPriority(ETH_IRQn, 5, 0);' + "`r`n" + '    HAL_NVIC_EnableIRQ(ETH_IRQn);' + "`r`n" + '  }' + "`r`n" + '}' + "`r`n"
  $msp = $msp -replace '(/\* USER CODE BEGIN 1 \*/)', ("`$1" + $e)
  Write-Text $mspPath $msp $encUtf8
  Write-Host '[FIXED] stm32f4xx_hal_msp.c: re-added HAL_ETH_MspInit'
} else {
  Write-Host '[OK]    stm32f4xx_hal_msp.c: HAL_ETH_MspInit present'
}

# ---- 7. la_sample.c: include dma.h ----
$laPath = Join-Path $root 'SystemServices\la_sample.c'
$la = Read-Text $laPath $encUtf8
if ($la -and $la -notmatch '#include "dma.h"') {
  $la = $la -replace '(#include "tim.h")', ("`$1`r`n#include `"dma.h`"")
  Write-Text $laPath $la $encUtf8
  Write-Host '[FIXED] la_sample.c: added #include "dma.h"'
} else {
  Write-Host '[OK]    la_sample.c: dma.h included'
}

# ---- 8. pinout.h: DEBUG_UART_IRQn -> USART3 ----
$pinPath = Join-Path $root 'Config\pinout.h'
$pin = Read-Text $pinPath $encGbk
if ($pin -and $pin -match 'USART2_IRQn') {
  $pin = $pin -replace 'USART2_IRQn', 'USART3_IRQn'
  Write-Text $pinPath $pin $encGbk
  Write-Host '[FIXED] pinout.h: DEBUG_UART_IRQn -> USART3_IRQn'
} else {
  Write-Host '[OK]    pinout.h: USART3_IRQn already set'
}

Write-Host 'fix_after_mx done.'
