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
#define EVENT_BUS_SUBS_MAX      8     // 每个消息类型最大订阅者数
#define EVENT_BUS_QUEUE_LENGTH  64    // 主事件队列深度
#define EVENT_BUS_POOL_SIZE     32    // 静态消息池槽位数
#define EVENT_BUS_MSG_MAX_PAYLOAD 128 // 单条消息 payload 最大长度（字节）

/*--------------------------- 系统定时器 ------------------------------------*/
#define SYS_TICK_1S_PERIOD_MS   1000
#define SYS_TICK_200MS_PERIOD_MS 200
#define KEY_SCAN_PERIOD_MS      10

#define DEVICE_I2C_TIMEOUT_MS   100

#define WDOG_FEED_PERIOD_MS     1000   // 喂狗周期，需小于 IWDG 超时的一半

/* ------------------ 上位机通信 (DataLink) ------------------ */
#define HOSTLINK_RX_DMA_BUF_SIZE    256     // DMA 接收缓冲
#define HOSTLINK_TX_STREAM_SIZE     512     // 发送流缓冲
#define HOSTLINK_TX_DMA_CHUNK       128     // 每次 DMA 发送块大小
#define HOSTLINK_CMD_QUEUE_LEN      16      // 命令队列深度（突发帧不丢）
#define HOSTLINK_MAX_VARS           64      // 最大注册变量数
#define HOSTLINK_MAX_SUBSCRIBE      16      // 最大订阅变量数
#define HOSTLINK_SAMPLE_PERIOD_MS   10      // 默认采集周期
#define HOSTLINK_CRC_POLY           0xA001  // CRC-16/MODBUS 多项式

/* 调试模式：1 = 关闭 IWDG 与任务级看门狗（供 gdb 断点调试，发布前务必置 0）。
 * 可用编译选项 -DAPP_DEBUG_MODE=1 覆盖（构建系统/CI 双模式验证）。 */
#ifndef APP_DEBUG_MODE
#define APP_DEBUG_MODE              0
#endif



#endif
