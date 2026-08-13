/* ================================================================
 * bsp_power —— 低功耗管理实现
 *
 * 关键安全约束：
 *   - IWDG 与 RTC 共用 LSI（未校准 17~47kHz）：RTC 1"秒" 始终占
 *     IWDG 预算约 25%（两振荡器同源同比），因此睡眠上限取 2 RTC 秒
 *     （=50% IWDG 预算），任何 LSI 频率下都安全；
 *   - tick 补偿按 RTC 秒数（与 RTC 同源），系统 tick 与 RTC 保持一致，
 *     墙钟漂移由 SNTP 周期校准；
 *   - 睡眠期间 CAN/ETH/UART 不接收：`power on` 仅限低功耗场景，
 *     默认关闭。
 * ================================================================ */
#include "bsp_power.h"
#include "FreeRTOS.h"
#include "task.h"
#include "iwdg.h"
#include "rtc.h"
#include "logger.h"

#include "stm32f4xx_hal.h"

/* STOP 唤醒后 HAL 不自动恢复时钟树：由 CubeMX 时钟配置函数重建 */
extern void SystemClock_Config(void);

#define POWER_MAX_SLEEP_MS    2000u   /* 2 RTC 秒（LSI 同源，安全） */
#define POWER_MIN_SLEEP_MS    2000u   /* 空闲 <2s 不值得进 STOP */

static volatile uint8_t s_power_enabled = 0;  /* STOP 模式总开关（默认关） */

/** @brief 空闲钩子（configUSE_IDLE_HOOK=1）：CPU 空闲时 WFI 停止旋转。
 *  任何中断（SysTick 1ms）都会唤醒，外设零影响，纯省功耗。 */
void vApplicationIdleHook(void)
{
    __WFI();
}

uint8_t BSP_Power_IsEnabled(void)
{
    return s_power_enabled;
}

int BSP_Power_Enable(void)
{
    if (s_power_enabled) {
        return 0;
    }
    __HAL_RCC_PWR_CLK_ENABLE();
    /* RTC 唤醒定时器：1Hz（LSI 分频），配 LSE/LSI 均可 */
    HAL_NVIC_SetPriority(RTC_WKUP_IRQn, 7, 0);   /* ≥5：可 FromISR（本工程不用） */
    HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);
    s_power_enabled = 1;
    LOG_Printf("[PWR] STOP tickless ON (idle>2s 时休眠；CAN/ETH 数据暂停)\r\n");
    return 0;
}

void BSP_Power_Disable(void)
{
    if (!s_power_enabled) {
        return;
    }
    s_power_enabled = 0;
    HAL_NVIC_DisableIRQ(RTC_WKUP_IRQn);
    LOG_Printf("[PWR] STOP tickless OFF (normal mode)\r\n");
}

/** @brief 初始化：PWR 时钟使能（STOP 入口需要） */
void BSP_Power_Init(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
}

/**
 * @brief  内核空闲路径：全任务阻塞时被 FreeRTOS 调用
 * @note   仅 `power on` 且空闲期足够长才进 STOP；否则立即返回（常规节拍）。
 *         休眠时长按 RTC 秒取整（LSI 同源，与 IWDG 预算比例恒定）。
 */
void BSP_Power_TicklessSleep(uint32_t xExpectedIdleTime)
{
    uint32_t sleep_s;

    if (!s_power_enabled || xExpectedIdleTime < POWER_MIN_SLEEP_MS) {
        return;
    }
    if (eTaskConfirmSleepModeStatus() == eAbortSleep) {
        return;
    }
    if (xExpectedIdleTime > POWER_MAX_SLEEP_MS) {
        xExpectedIdleTime = POWER_MAX_SLEEP_MS;
    }
    sleep_s = xExpectedIdleTime / 1000u;   /* RTC 秒粒度向下取整 */
    if (sleep_s == 0u) {
        return;
    }

    /* 停 SysTick：STOP 期间内核时钟停止，节拍暂停 */
    HAL_SuspendTick();

    /* 喂 IWDG：STOP 期间 LSI 继续走，最长睡 2 RTC 秒（≤50% 预算） */
    HAL_IWDG_Refresh(&hiwdg);

    /* 配置 RTC 唤醒（清残留标志 + 使能 EXTI 唤醒线） */
    __HAL_RTC_WAKEUPTIMER_EXTI_ENABLE_IT();
    (void)HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, sleep_s,
                                      RTC_WAKEUPCLOCK_CK_SPRE_16BITS);

    /* 进入 STOP（WFI）；RTC 唤醒中断返回后从此行继续 */
    HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);

    /* 醒来：关唤醒线、恢复 SysTick、补休眠期间的系统 tick */
    __HAL_RTC_WAKEUPTIMER_EXTI_DISABLE_IT();
    SystemClock_Config();              /* 重建时钟树（HSE→PLL→168MHz） */
    HAL_ResumeTick();
    vTaskStepTick(sleep_s * 1000u);
}
