#ifndef BSP_UART_H
#define BSP_UART_H

#include <stdint.h>

/* 串口通道标识 */
typedef enum {
    BSP_UART_DBG,       /* 调试/日志串口 */
    BSP_UART_HOST,      /* 上位机 HOSTLINK 串口 */
    BSP_UART_COUNT
} bsp_uart_id_t;

/* RX 空闲中断回调（ISR 上下文）：
 * data/len 指向本次收到的连续数据，返回后 BSP 自动重启 DMA 接收。 */
typedef void (*bsp_uart_rx_cb_t)(bsp_uart_id_t id, const uint8_t *data,
                                 uint16_t len, void *ctx);

/* TX DMA 完成回调（ISR 上下文） */
typedef void (*bsp_uart_tx_cb_t)(bsp_uart_id_t id, void *ctx);

/* 初始化串口通道（需先完成平台外设初始化，如 MX_USARTx_UART_Init） */
void BSP_UART_Init(bsp_uart_id_t id);

/* 注册 RX/TX 事件回调（可在任务上下文调用） */
void BSP_UART_RegisterRxCb(bsp_uart_id_t id, bsp_uart_rx_cb_t cb, void *ctx);
void BSP_UART_RegisterTxCb(bsp_uart_id_t id, bsp_uart_tx_cb_t cb, void *ctx);

/* 启动 DMA 接收（buf 必须保持有效直至下次启动） */
void BSP_UART_RxStart(bsp_uart_id_t id, uint8_t *buf, uint16_t len);

/* 停止 DMA 接收 */
void BSP_UART_RxStop(bsp_uart_id_t id);

/* DMA 发送。返回 0 成功；-1 表示上次发送未完成。 */
int BSP_UART_TransmitDMA(bsp_uart_id_t id, const uint8_t *data, uint16_t len);

/* 供串口中断处理调用（已包含 HAL_UART_IRQHandler + IDLE 处理） */
void BSP_UART_IRQHandler(bsp_uart_id_t id);

/* 仅处理 IDLE 事件：用于 CubeMX 生成代码已调用 HAL_UART_IRQHandler 的场景 */
void BSP_UART_IdleISR(bsp_uart_id_t id);

#endif
