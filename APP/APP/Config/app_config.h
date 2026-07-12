#ifndef APP_CONFIG_H
#define APP_CONFIG_H


#define DEBUG_APP 1
// 日志系统
#define LOG_TX_STREAM_SIZE      2048
#define LOG_RX_STREAM_SIZE      1024
#define LOG_RX_DMA_BUF_SIZE     256
#define LOG_TX_DMA_CHUNK        128

// Shell
#define SHELL_LINE_MAX          128
#define SHELL_CMD_MAX           20

// 事件总线（预留）
#define EVENT_BUS_MAX_EVENTS    64
#define EVENT_BUS_SUBS_MAX      4

#endif
