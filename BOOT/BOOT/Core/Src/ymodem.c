#include "ymodem.h"
#include "crc32.h"
#include <string.h>
#include <stdio.h>      // 用于 sscanf

/* 静态助手函数声明 */
static void send_byte(uint8_t ch);
static int  wait_byte(uint32_t timeout);
static int  recv_packet(uint8_t *buf, uint16_t size, uint32_t timeout);
static void cancel_transfer(void);
static uint32_t calc_frame_crc(const uint8_t *data, uint16_t len);
static int parse_file_info(const uint8_t *info, ymodem_t *ctx);

/* 全局上下文指针，便于回调访问 */
static ymodem_t *g_ymodem_ctx = NULL;

/**
 * @brief  接收一个完整的Ymodem文件
 * @param  ctx        上下文指针
 * @param  write_addr Flash写入起始地址（本版暂不使用，仅做存储记录）
 * @retval ymodem_status_t
 */
ymodem_status_t ymodem_receive_file(ymodem_t *ctx, uint32_t write_addr) {
    uint8_t buf[FULL_FRAME_SIZE];
    uint32_t calc_crc;
    int32_t ch;
    uint16_t retry;
    uint16_t seq, seq_comp;
    uint16_t data_len = PACKET_SIZE;

    g_ymodem_ctx = ctx;
    ctx->flash_addr = write_addr;
    ctx->received_size = 0;
    ctx->packet_seq = 1;
    crc32_init(&ctx->current_crc);

    /* ===== 第一阶段：发起传输，等待文件信息帧 ===== */
    printf("[Ymodem] Starting file info phase...\r\n");
    for (int i = 0; i < 5; i++) {
        send_byte('C');
        HAL_Delay(5);
    }

    retry = 0;
    while (1) {
        ch = wait_byte(3000);   // 3秒超时
        if (ch == SOH || ch == STX) {
            /* 收到信息帧，读取整个帧 */
            buf[0] = (uint8_t)ch;
            if (recv_packet(buf + 1, FILE_INFO_SIZE + PACKET_HEADER + PACKET_CRC_SIZE - 1, 1000) <= 0) {
                printf("[Ymodem] Timeout reading file info packet.\r\n");
                return YMODEM_ERR_TIMEOUT;
            }
            /* 校验CRC（信息帧数据长度128，CRC在最后4字节） */
            calc_crc = calc_frame_crc(buf + PACKET_HEADER, FILE_INFO_SIZE);
            uint32_t rcv_crc = *(uint32_t *)(buf + PACKET_HEADER + FILE_INFO_SIZE);
            if (calc_crc != rcv_crc) {
                printf("[Ymodem] File info CRC error. calc:0x%08X, recv:0x%08X\r\n", calc_crc, rcv_crc);
                send_byte(NAK);
                retry++;
                if (retry > MAX_RETRY) return YMODEM_ERR_CRC;
                continue;
            }
            /* 解析文件名、大小、期望CRC */
            if (parse_file_info(buf + PACKET_HEADER, ctx) != 0) {
                printf("[Ymodem] Failed to parse file info.\r\n");
                send_byte(CAN);
                return YMODEM_ERR_FILE;
            }
            printf("[Ymodem] File: %s, Size: %lu, Expected CRC: 0x%08X\r\n",
                   ctx->file_name, ctx->file_size, ctx->file_crc);
            send_byte(ACK);
            send_byte('C');   // 请求数据
            break;            // 跳出文件信息阶段
        }
        else if (ch == EOT) {
            /* 空文件传输（无文件） */
            printf("[Ymodem] Empty transfer.\r\n");
            send_byte(ACK);
            return YMODEM_OK;
        }
        else if (ch < 0) {
            /* 超时 */
            printf("[Ymodem] Timeout waiting for file info.\r\n");
            return YMODEM_ERR_TIMEOUT;
        }
        else {
            /* 干扰字符，继续发'C' */
            send_byte('C');
            retry++;
            if (retry > MAX_RETRY) return YMODEM_ERR_TIMEOUT;
        }
    }

    /* ===== 第二阶段：接收数据帧 ===== */
    printf("[Ymodem] Data phase start.\r\n");
    retry = 0;
    while (1) {
        ch = wait_byte(5000);   // 等待帧头
        if (ch == STX) {
            data_len = PACKET_SIZE;
        } else if (ch == SOH) {
            data_len = 128;
        } else if (ch == EOT) {
            /* 文件传输结束 */
            printf("[Ymodem] EOT received.\r\n");
            send_byte(NAK);     // 要求重发EOT（标准流程）
            ch = wait_byte(3000);
            if (ch == EOT) {
                send_byte(ACK);
                send_byte('C');
                /* 等待结束帧（空文件名SOH） */
                ch = wait_byte(3000);
                if (ch == SOH) {
                    buf[0] = SOH;
                    recv_packet(buf+1, FILE_INFO_SIZE + PACKET_HEADER + PACKET_CRC_SIZE - 1, 1000);
                    send_byte(ACK);
                }
                break;
            } else {
                printf("[Ymodem] EOT retry failed.\r\n");
                send_byte(CAN);
                return YMODEM_ERR_CANCEL;
            }
        } else if (ch < 0) {
            printf("[Ymodem] Timeout waiting for data.\r\n");
            if (++retry > MAX_RETRY) return YMODEM_ERR_TIMEOUT;
            send_byte(NAK);
            continue;
        } else {
            /* 未知帧头 */
            printf("[Ymodem] Unexpected char: 0x%02X\r\n", ch);
            send_byte(NAK);
            retry++;
            if (retry > MAX_RETRY) return YMODEM_ERR_CANCEL;
            continue;
        }

        /* 读取剩余帧：序号 + 补码 + 数据 + CRC */
        uint16_t expected_len = PACKET_HEADER + data_len + PACKET_CRC_SIZE - 1;
        if (recv_packet(buf + 1, expected_len, 2000) != expected_len) {
            printf("[Ymodem] Packet read timeout.\r\n");
            send_byte(NAK);
            retry++;
            if (retry > MAX_RETRY) return YMODEM_ERR_TIMEOUT;
            continue;
        }

        /* 获取序号和补码 */
        seq      = buf[1];
        seq_comp = buf[2];

        /* 序号检查 */
        if ((uint8_t)(seq + seq_comp) != 0xFF) {
            printf("[Ymodem] Sequence number error.\r\n");
            send_byte(NAK);
            retry++;
            if (retry > MAX_RETRY) return YMODEM_ERR_SEQ;
            continue;
        }
        if (seq != ctx->packet_seq) {
            printf("[Ymodem] Seq mismatch: expected %d, got %d\r\n", ctx->packet_seq, seq);
            /* 如果是重复上一包，仍应ACK */
            if (seq == (uint8_t)(ctx->packet_seq - 1)) {
                send_byte(ACK);
                continue;
            }
            send_byte(NAK);
            retry++;
            if (retry > MAX_RETRY) return YMODEM_ERR_SEQ;
            continue;
        }

        /* 校验帧CRC32 */
        calc_crc = calc_frame_crc(buf + PACKET_HEADER, data_len);
        uint32_t recv_crc = *(uint32_t *)(buf + PACKET_HEADER + data_len);
        if (calc_crc != recv_crc) {
            printf("[Ymodem] Frame CRC error. calc:0x%08X, recv:0x%08X\r\n", calc_crc, recv_crc);
            send_byte(NAK);
            retry++;
            if (retry > MAX_RETRY) return YMODEM_ERR_CRC;
            continue;
        }

        /* 数据有效，更新总体CRC和接收大小 */
        crc32_update(&ctx->current_crc, buf + PACKET_HEADER, data_len);
        ctx->received_size += data_len;
        ctx->packet_seq++;

        printf("[Ymodem] Packet %d OK, total %lu/%lu\r\n", seq, ctx->received_size, ctx->file_size);

        /* 这里将数据写入Flash（暂不实现，仅打印） */
        // write_flash(ctx->flash_addr, buf + PACKET_HEADER, data_len);
        ctx->flash_addr += data_len;

        retry = 0;
        send_byte(ACK);
    }

    /* ===== 第三阶段：校验整体CRC32 ===== */
    uint32_t final_crc = crc32_finalize(&ctx->current_crc);
    printf("[Ymodem] File CRC: 0x%08X, expected: 0x%08X\r\n", final_crc, ctx->file_crc);
    if (final_crc != ctx->file_crc) {
        printf("[Ymodem] File CRC mismatch!\r\n");
        return YMODEM_ERR_CRC;
    }

    printf("[Ymodem] Transfer complete successfully.\r\n");
    return YMODEM_OK;
}

/* ========== 内部函数实现 ========== */

static void send_byte(uint8_t ch) {
    ymodem_send_char(ch);
}

static int wait_byte(uint32_t timeout) {
    uint8_t ch;
    if (ymodem_recv_byte(&ch, timeout) == 0)
        return ch;
    return -1;
}

static int recv_packet(uint8_t *buf, uint16_t size, uint32_t timeout) {
    uint32_t start = HAL_GetTick();
    uint16_t rcvd = 0;
    while (rcvd < size) {
        if (HAL_GetTick() - start > timeout)
            break;
        uint8_t ch;
        if (ymodem_recv_byte(&ch, 10) == 0) {
            buf[rcvd++] = ch;
            start = HAL_GetTick();   // 重置超时
        }
    }
    return rcvd;
}

static void cancel_transfer(void) {
    for (int i = 0; i < 5; i++)
        send_byte(CAN);
}

/**
 * @brief  计算帧内数据的CRC32（不包含帧头、序号、补码）
 */
static uint32_t calc_frame_crc(const uint8_t *data, uint16_t len) {
    uint32_t crc;
    crc32_init(&crc);
    crc32_update(&crc, data, len);
    return crc32_finalize(&crc);
}

/**
 * @brief  解析文件信息帧：格式 "filename\0size_hex CRC32_hex\0"
 *         例如："app.bin\0 0x1234 0xAABBCCDD\0"
 * @retval 0 成功，-1 失败
 */
static int parse_file_info(const uint8_t *info, ymodem_t *ctx) {
    /* 提取文件名 */
    int i = 0;
    while (info[i] != 0x00 && i < FILE_INFO_SIZE) {
        ctx->file_name[i] = info[i];
        i++;
    }
    ctx->file_name[i] = '\0';
    i++; // 跳过结束符

    /* 提取文件大小（十六进制字符串） */
    char size_str[16] = {0};
    int j = 0;
    while (info[i] == ' ') i++; // 跳过空格
    while (info[i] != ' ' && info[i] != 0x00 && j < 15) {
        size_str[j++] = info[i++];
    }
    ctx->file_size = strtoul(size_str, NULL, 16);

    /* 提取期望CRC32 */
    while (info[i] == ' ') i++;
    char crc_str[16] = {0};
    j = 0;
    while (info[i] != ' ' && info[i] != 0x00 && j < 15) {
        crc_str[j++] = info[i++];
    }
    ctx->file_crc = strtoul(crc_str, NULL, 16);

    return 0;
}