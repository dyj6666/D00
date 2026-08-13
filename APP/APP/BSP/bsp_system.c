/* ================================================================
 * bsp_system —— 系统服务：复位/延时/备份域寄存器
 *
 * 架构位置：APP BSP 层；OTA 升级标志与系统复位统一入口
 * ================================================================ */
#include "bsp_system.h"
#include "stm32f4xx_hal.h"

void BSP_SystemReset(void)
{
    NVIC_SystemReset();
}

void BSP_DelayMs(uint32_t ms)
{
    HAL_Delay(ms);
}

uint32_t BSP_GetTick(void)
{
    return HAL_GetTick();
}

void BSP_DWT_Enable(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t BSP_DWT_GetCycleCount(void)
{
    return DWT->CYCCNT;
}

bsp_reset_reason_t BSP_GetResetReason(void)
{
    uint32_t flags = RCC->CSR;
    bsp_reset_reason_t reason = BSP_RESET_UNKNOWN;

    if (flags & RCC_CSR_IWDGRSTF) {
        reason = BSP_RESET_IWDG;
    } else if (flags & RCC_CSR_WWDGRSTF) {
        reason = BSP_RESET_WWDG;
    } else if (flags & RCC_CSR_PORRSTF) {
        reason = BSP_RESET_POWER_ON;
    } else if (flags & RCC_CSR_PINRSTF) {
        reason = BSP_RESET_PIN;
    } else if (flags & RCC_CSR_SFTRSTF) {
        reason = BSP_RESET_SOFTWARE;
    }

    RCC->CSR |= RCC_CSR_RMVF;   /* 清除复位标志，供下次读取 */
    return reason;
}
