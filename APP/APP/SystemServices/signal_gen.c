#include "signal_gen.h"

#include <string.h>

#include "cmsis_os2.h"
#include "stm32f4xx_hal.h"

#define SG_STACK_SIZE  1024
#define SG_FLAG_STOP   0x01

static UART_HandleTypeDef sg_huart6;
static osThreadId_t       sg_task_id = NULL;
static volatile uint8_t   sg_running = 0;
static uint8_t            sg_data[SG_TEXT_MAX];
static uint16_t           sg_len = 0;
static uint16_t           sg_interval_ms = 5;

static void sg_uart_task(void *arg)
{
    (void)arg;
    for (;;) {
        HAL_UART_Transmit(&sg_huart6, sg_data, sg_len, HAL_MAX_DELAY);
        uint32_t flags = osThreadFlagsWait(SG_FLAG_STOP, osFlagsWaitAny,
                                           sg_interval_ms);
        if (flags & SG_FLAG_STOP) {
            break;
        }
    }
    sg_running = 0;
    osThreadExit();
}

static int sg_start_common(uint32_t baud, uint16_t interval_ms)
{
    if (baud < SG_BAUD_MIN || baud > SG_BAUD_MAX) {
        return -1;
    }
    if (sg_running) {
        SG_UartStop();
    }

    __HAL_RCC_USART6_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* PC6 -> USART6_TX (AF8)，覆盖 TIM8_CH1 的 PWM 复用 */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_6;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF8_USART6;
    HAL_GPIO_Init(GPIOC, &gpio);

    memset(&sg_huart6, 0, sizeof(sg_huart6));
    sg_huart6.Instance = USART6;
    sg_huart6.Init.BaudRate = baud;
    sg_huart6.Init.WordLength = UART_WORDLENGTH_8B;
    sg_huart6.Init.StopBits = UART_STOPBITS_1;
    sg_huart6.Init.Parity = UART_PARITY_NONE;
    sg_huart6.Init.Mode = UART_MODE_TX;
    sg_huart6.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    sg_huart6.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&sg_huart6) != HAL_OK) {
        __HAL_RCC_USART6_CLK_DISABLE();
        return -2;
    }

    sg_interval_ms = interval_ms;
    sg_running = 1;

    osThreadAttr_t attr = {
        .name = "SG_UART",
        .stack_size = SG_STACK_SIZE,
        .priority = osPriorityLow,
    };
    sg_task_id = osThreadNew(sg_uart_task, NULL, &attr);
    if (sg_task_id == NULL) {
        sg_running = 0;
        HAL_UART_DeInit(&sg_huart6);
        __HAL_RCC_USART6_CLK_DISABLE();
        return -3;
    }
    return 0;
}

static int sg_hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int SG_UartStart(uint32_t baud, const char *text, uint16_t interval_ms)
{
    size_t len;
    if (text == NULL) return -1;
    len = strlen(text);
    if (len == 0 || len >= SG_TEXT_MAX) return -1;
    memcpy(sg_data, text, len);
    sg_len = (uint16_t)len;
    return sg_start_common(baud, interval_ms);
}

int SG_UartStartHex(uint32_t baud, const char *hex, uint16_t interval_ms)
{
    size_t len;
    if (hex == NULL) return -1;
    len = strlen(hex);
    if (len == 0 || len % 2 != 0 || len / 2 >= SG_TEXT_MAX) return -1;
    for (size_t i = 0; i < len / 2; i++) {
        int hi = sg_hex_value(hex[i * 2]);
        int lo = sg_hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        sg_data[i] = (uint8_t)((hi << 4) | lo);
    }
    sg_len = (uint16_t)(len / 2);
    return sg_start_common(baud, interval_ms);
}

void SG_UartStop(void)
{
    if (!sg_running && sg_task_id == NULL) {
        return;
    }
    if (sg_task_id != NULL) {
        osThreadFlagsSet(sg_task_id, SG_FLAG_STOP);
        sg_task_id = NULL;
    }
    /* 等待任务退出当前发送帧（< 5ms @ 115200/64B），避免与后续 Start 竞态 */
    osDelay(20);
}

uint8_t SG_UartIsRunning(void)
{
    return sg_running;
}
