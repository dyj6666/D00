/**
 * @file    ymodem.h
 * @brief   Ymodem protocol receiver state machine interface
 */

#ifndef YMODEM_H
#define YMODEM_H

#include <stdint.h>

/* Protocol constants */
#define YMODEM_SOH              0x01
#define YMODEM_STX              0x02
#define YMODEM_EOT              0x04
#define YMODEM_ACK              0x06
#define YMODEM_NAK              0x15
#define YMODEM_CAN              0x18
#define YMODEM_C                0x43

#define YMODEM_PACKET_SIZE      1024
#define YMODEM_FILE_INFO_SIZE   128
#define YMODEM_MAX_RETRY        10

/* Status codes */
typedef enum {
    YMODEM_OK = 0,
    YMODEM_ERR_TIMEOUT,
    YMODEM_ERR_CRC,
    YMODEM_ERR_SEQ,
    YMODEM_ERR_CANCEL,
    YMODEM_ERR_FILE,
    YMODEM_ERR_FLASH,
    YMODEM_ERR_INTERNAL
} ymodem_status_t;

/* Transfer context */
typedef struct {
    char     file_name[64];
    uint32_t file_size;
    uint32_t file_crc;          /* expected overall CRC32 */
    uint32_t received_size;
    uint32_t current_crc;
    uint16_t packet_seq;
    uint32_t write_addr;        /* current flash write address in Download area */
} ymodem_ctx_t;

ymodem_status_t ymodem_receive(ymodem_ctx_t *ctx, uint32_t flash_addr);

#endif /* YMODEM_H */





