/* ================================================================
 * uart.h —— 外部串口（备用 UART，默认 UART1 @ IO6(TX)/IO5(RX)）
 *
 * 用途：与另一块 MCU（例如 STM32）做板间通信。
 * 注意：
 *   - UART0 已经被 IDF 控制台占用（接板载 CH343P，打印日志/下载），
 *     本模块不碰 UART0；
 *   - 采用"驱动缓冲区 + 事件队列"模型：收到数据时队列里出现
 *     UART_DATA 事件，任务再 bsp_uart_read() 取走数据 —— 不需要轮询，
 *     也不会在中断里做重活。
 * ================================================================ */
#ifndef __BSP_UART_H
#define __BSP_UART_H

#include "bsp_types.h"
#include "bsp_pinmap.h"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 串口配置 */
typedef struct
{
    uart_port_t port;           /*!< 端口号（默认 UART_NUM_1） */
    uint32_t    baud_rate;      /*!< 波特率（默认 115200） */
    gpio_num_t  tx_gpio;        /*!< TX 引脚 */
    gpio_num_t  rx_gpio;        /*!< RX 引脚 */
    size_t      rx_buf_size;    /*!< 驱动接收缓冲（默认 2048） */
    size_t      tx_buf_size;    /*!< 驱动发送缓冲（默认 1024） */
    uint8_t     event_queue_len;/*!< 事件队列长度（默认 16） */
} bsp_uart_cfg_t;

/** 默认配置：UART1 / 115200 / IO6-TX IO5-RX */
#define BSP_UART_CFG_DEFAULT                                            \
    {                                                                   \
        .port = (uart_port_t)BSP_AUX_UART_PORT,                         \
        .baud_rate = BSP_AUX_UART_BAUD,                                 \
        .tx_gpio = BSP_AUX_UART_TX_GPIO,                                \
        .rx_gpio = BSP_AUX_UART_RX_GPIO,                                \
        .rx_buf_size = 2048U,                                           \
        .tx_buf_size = 1024U,                                           \
        .event_queue_len = 16U,                                         \
    }

/**
 * @brief  覆盖默认配置（必须在 bsp_uart_init 之前调用）
 */
esp_err_t bsp_uart_configure(const bsp_uart_cfg_t *cfg);

/**
 * @brief  初始化并安装串口驱动（幂等）
 */
esp_err_t bsp_uart_init(void);

/**
 * @brief  卸载串口驱动
 */
esp_err_t bsp_uart_deinit(void);

/**
 * @brief  发送数据（阻塞直到写入驱动缓冲）
 * @return 实际写入的字节数（< 0 表示失败）
 */
int bsp_uart_write(const void *data, size_t len, uint32_t timeout_ms);

/**
 * @brief  接收数据
 * @return 实际读到的字节数（0 表示超时）
 */
int bsp_uart_read(void *buf, size_t len, uint32_t timeout_ms);

/**
 * @brief  清空接收缓冲
 */
esp_err_t bsp_uart_flush(void);

/**
 * @brief  获取事件队列（配合 xQueueReceive 使用，事件类型见 uart_event_type_t）
 */
QueueHandle_t bsp_uart_event_queue(void);

/**
 * @brief  动态修改波特率
 */
esp_err_t bsp_uart_set_baudrate(uint32_t baud_rate);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_UART_H */
