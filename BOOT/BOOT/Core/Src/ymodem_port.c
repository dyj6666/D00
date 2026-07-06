// ymodem_port.c
#include "ymodem_port.h"
#include "main.h"
#include "iwdg.h"

extern UART_HandleTypeDef huart1;
static fifo_t ymodem_fifo;

/* 在 main.c 或 stm32f4xx_it.c 的 USART1_IRQHandler 中调用此函数 */
void ymodem_uart_isr_handler(void) {
    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE)) {
        uint8_t byte = (uint8_t)(huart1.Instance->DR);
        fifo_put(&ymodem_fifo, byte);
    }
    HAL_UART_IRQHandler(&huart1); // 需要这一行保持 HAL 状态同步
}

void ymodem_port_init(void) {
    /* 1. 停止可能存在的 DMA 及 IDLE 中断 */
    HAL_UART_AbortReceive(&huart1);
    __HAL_UART_DISABLE_IT(&huart1, UART_IT_IDLE);
    
    /* 2. 重新初始化 UART 为 RXNE 中断模式（不启动 DMA） */
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart1);
    
    /* 3. 使能 RXNE 中断 */
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);
    
    /* 4. 清空 FIFO */
    fifo_init(&ymodem_fifo);
}

void ymodem_send_byte(uint8_t byte) {
    HAL_UART_Transmit(&huart1, &byte, 1, 100);
}

int32_t ymodem_read_byte(uint32_t timeout_ms) {
    uint32_t start = ymodem_get_tick();
    uint8_t byte;
    while (!fifo_get(&ymodem_fifo, &byte)) {
        if ((ymodem_get_tick() - start) >= timeout_ms) {
            return -1;  // 超时
        }
        ymodem_feed_watchdog();  // 等待期间喂狗
    }
    return byte;
}

void ymodem_feed_watchdog(void) {
    HAL_IWDG_Refresh(&hiwdg);
}

uint32_t ymodem_get_tick(void) {
    return HAL_GetTick();
}

