#ifndef APP_CONFIG_H
#define APP_CONFIG_H


#define DEBUG_APP 0

#define BOOT_FLAG_UPGRADE  0x5A5A

// 日志系统
#define LOG_TX_STREAM_SIZE      2048
#define LOG_RX_STREAM_SIZE      1024
#define LOG_RX_DMA_BUF_SIZE     256
#define LOG_TX_DMA_CHUNK        128

// Shell
#define SHELL_LINE_MAX          128
#define SHELL_CMD_MAX           20

/*--------------------------- 事件总线 --------------------------------------*/
#define EVENT_BUS_MAX_EVENTS    64
#define EVENT_BUS_SUBS_MAX      4
#define EVENT_BUS_QUEUE_LENGTH  32      // 新增：事件队列深度

/*--------------------------- 系统定时器 ------------------------------------*/
#define SYS_TICK_1S_PERIOD_MS   1000
#define SYS_TICK_200MS_PERIOD_MS 200
#define KEY_SCAN_PERIOD_MS      10

#define DEVICE_I2C_TIMEOUT_MS   100

#define WDOG_FEED_PERIOD_MS     1000   // 喂狗周期，需小于 IWDG 超时的一半

/* 上位机通信 */
#define HOSTLINK_RX_DMA_BUF_SIZE    1024
#define HOSTLINK_TX_DMA_BUF_SIZE    512
#define HOSTLINK_MAX_VARS           64      /* 最大注册变量数 */
#define HOSTLINK_MAX_SUBSCRIBE      32      /* 最大订阅变量数 */
#define HOSTLINK_SAMPLE_PERIOD_MS   10      /* 默认采集周期 10ms */

#define HOSTLINK_TX_STREAM_SIZE      2048   // 发送流缓冲区
#define HOSTLINK_TX_DMA_CHUNK         128
#define HOSTLINK_CRC_POLY             0x8005 // CRC-16/MODBUS 多项式

#endif
