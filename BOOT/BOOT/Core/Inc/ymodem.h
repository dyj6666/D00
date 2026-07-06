/**
 * @file    ymodem.h
 * @brief   Ymodem 协议接收端状态机接口 
 * 
 * @details 本模块实现了 Ymodem 协议的完整接收逻辑，采用事件驱动的层次化状态机。
 *          所有硬件相关操作均抽象到 ymodem_port.h 中，用户只需实现四个函数即可
 *          将本引擎适配到任意物理接口 (USART/CAN/ETH/SPI 等)。
 * 
 *          协议特性：
 *          - 支持标准 Ymodem 文件传输，使用 1024 字节数据帧 (STX)
 *          - 每帧携带 32 位 CRC 校验，传输完成后额外进行文件级 CRC32 比对
 *          - 完全非阻塞设计，所有超时均可配置，等待期间自动喂狗
 *          - 自动处理序号错乱、重复帧、CRC 错误和超时重传
 *          - 支持传输取消 (CAN 序列) 和空文件传输
 * 
 * @note    硬件抽象层接口定义见 ymodem_port.h，需用户实现。
 *          移植时只需实现 ymodem_port.c 中的函数即可，本模块代码无需修改。
 * 
 * @version 1.0.0
 * @date    2026-07-07
 * @author  Industrial OTA Team
 */

#ifndef YMODEM_H
#define YMODEM_H

#include <stdint.h>

/*===========================================================================
 * 协议常量定义
 *===========================================================================*/
#define YMODEM_SOH              0x01    /**< 帧头：128 字节数据包 (用于文件信息帧或小数据) */
#define YMODEM_STX              0x02    /**< 帧头：1024 字节数据包 (标准数据传输) */
#define YMODEM_EOT              0x04    /**< 传输结束标志 */
#define YMODEM_ACK              0x06    /**< 确认应答 */
#define YMODEM_NAK              0x15    /**< 不确认应答 (请求重传当前数据包) */
#define YMODEM_CAN              0x18    /**< 取消传输 (连续发送 5 个视为终止) */
#define YMODEM_C                0x43    /**< ASCII 'C'，启动传输请求 (表示支持 CRC32) */

#define YMODEM_PACKET_SIZE      1024    /**< 数据帧有效载荷大小 (STX) */
#define YMODEM_FILE_INFO_SIZE   128     /**< 文件信息帧数据区大小 (SOH) */
#define YMODEM_MAX_RETRY        10      /**< 最大重传次数 (单个阶段) */

/*===========================================================================
 * 类型定义
 *===========================================================================*/

/** 
 * @brief Ymodem 传输状态码
 */
typedef enum {
    YMODEM_OK               = 0,    /**< 传输成功，文件 CRC 校验通过 */
    YMODEM_ERR_TIMEOUT      = 1,    /**< 通信超时 (等待数据或握手失败) */
    YMODEM_ERR_CRC          = 2,    /**< 帧 CRC 或文件整体 CRC 校验失败 */
    YMODEM_ERR_SEQ          = 3,    /**< 包序号错误 (非预期序号且非重复帧) */
    YMODEM_ERR_CANCEL       = 4,    /**< 传输被发送方或接收方取消 (CAN 序列) */
    YMODEM_ERR_FILE         = 5,    /**< 文件信息解析失败 (格式错误) */
    YMODEM_ERR_FLASH        = 6,    /**< Flash 写入错误 (保留，当前版本未实现) */
    YMODEM_ERR_INTERNAL     = 7     /**< 内部状态错误 (非法状态) */
} ymodem_status_t;

/** 
 * @brief Ymodem 传输上下文
 * 
 * @note  本结构体由调用者分配，并在 ymodem_receive() 过程中动态更新。
 *        传输完成后可从中读取文件名、文件大小、实际接收大小等信息。
 */
typedef struct {
    char     file_name[64];          /**< 接收到的文件名 (以 '\0' 结尾) */
    uint32_t file_size;              /**< 文件总大小 (字节，来自文件信息帧) */
    uint32_t file_crc;               /**< 期望的文件整体 CRC32 (由发送方提供) */
    uint32_t received_size;          /**< 已接收的数据字节数 (不含填充) */
    uint32_t current_crc;            /**< 实时计算的累积 CRC32 值 */
    uint16_t packet_seq;             /**< 期望的下一个数据包序号 (从 1 开始) */
} ymodem_ctx_t;

/*===========================================================================
 * 公开接口函数
 *===========================================================================*/

/**
 * @brief   启动 Ymodem 接收流程
 * 
 * @details 该函数会阻塞当前线程，直到传输完成、出错或取消。
 *          内部实现了完整的状态机，自动处理握手、数据接收、校验及结束。
 *          传输过程中会频繁调用 ymodem_feed_watchdog() 防止看门狗复位。
 * 
 *          使用前需确保：
 *          1. 已调用 ymodem_port_init() 初始化物理接口 (如 USART)；
 *          2. 已实现 ymodem_port.h 中的四个抽象函数；
 *          3. 看门狗已启动，且回调函数 ymodem_feed_watchdog() 正确喂狗。
 * 
 * @param   ctx         上下文指针 (需由调用者分配内存，可位于栈或静态区)
 * @param   flash_addr  接收到的数据存储起始地址 (当前版本仅记录，不写 Flash)
 * 
 * @retval  YMODEM_OK           传输成功，文件 CRC 校验通过
 * @retval  YMODEM_ERR_CRC      文件整体 CRC 校验失败 (数据已接收但完整性受损)
 * @retval  YMODEM_ERR_TIMEOUT  握手或数据传输阶段超时
 * @retval  其他               其他错误 (参见 ymodem_status_t)
 * 
 * @note    当前版本未实现 Flash 写入，因此 flash_addr 仅作为上下文记录。
 *          后续可扩展 state_rx_frame() 中的数据处理部分，将有效数据写入 Flash。
 * 
 * @code
 *   ymodem_ctx_t ctx;
 *   ymodem_status_t status = ymodem_receive(&ctx, DOWNLOAD_AREA_ADDR);
 *   if (status == YMODEM_OK) {
 *       printf("Received %s (%lu bytes)\n", ctx.file_name, ctx.file_size);
 *   }
 * @endcode
 */
ymodem_status_t ymodem_receive(ymodem_ctx_t *ctx, uint32_t flash_addr);

#endif /* YMODEM_H */





