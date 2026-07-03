#ifndef __YMODEM_H
#define __YMODEM_H

#include <stdint.h>

#define SOH   0x01
#define STX   0x02
#define EOT   0x04
#define ACK   0x06
#define NAK   0x15
#define CAN   0x18

#define PACKET_SIZE     1024
#define PACKET_HEADER   3
#define PACKET_CRC_SIZE 4
#define FULL_FRAME_SIZE (PACKET_HEADER + PACKET_SIZE + PACKET_CRC_SIZE)

#define FILE_INFO_SIZE  128

#define MAX_RETRY       10      // 最大重传次数

typedef enum {
    YMODEM_OK = 0,
    YMODEM_ERR_TIMEOUT,
    YMODEM_ERR_CRC,
    YMODEM_ERR_SEQ,
    YMODEM_ERR_CANCEL,
    YMODEM_ERR_FILE,
    YMODEM_ERR_FLASH
} ymodem_status_t;

typedef struct {
    char     file_name[64];
    uint32_t file_size;
    uint32_t file_crc;          // 期望的整个文件 CRC32
    uint32_t received_size;
    uint32_t current_crc;
    uint16_t packet_seq;
    uint32_t flash_addr;
} ymodem_t;

ymodem_status_t ymodem_receive_file(ymodem_t *ctx, uint32_t write_addr);

/* 底层通信接口，需用户实现 */
extern int ymodem_send_char(uint8_t ch);
extern int ymodem_recv_byte(uint8_t *ch, uint32_t timeout_ms);

#endif