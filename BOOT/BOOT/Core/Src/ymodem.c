/**
 * @file    ymodem.c
 * @brief   Ymodem protocol receiver state machine (Industrial Grade Final)
 *
 * @details
 *   - Non-blocking, fully event-driven FSM
 *   - Writes valid payload to Download flash area on the fly
 *   - Proper handling of padding bytes for file CRC
 *   - Detailed English log output
 *   - Zero warnings
 */

#include "ymodem.h"
#include "ymodem_port.h"
#include "crc32.h"
#include "flash_if.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*---------------------------------------------------------------------------
 * Internal macros
 *---------------------------------------------------------------------------*/
#define FRAME_BUF_SIZE      (3 + YMODEM_PACKET_SIZE + 4)
#define POLL_TIMEOUT_MS     50
#define FILE_INFO_TIMEOUT   3000
#define DATA_TIMEOUT        5000
#define EOT_TIMEOUT         3000

/*---------------------------------------------------------------------------
 * Internal types
 *---------------------------------------------------------------------------*/
typedef enum {
    STATE_INIT,
    STATE_WAIT_FILE_INFO,
    STATE_DATA_PHASE,
    STATE_RX_FRAME,
    STATE_EOT_PHASE,
    STATE_COMPLETE,
    STATE_ERROR
} fsm_state_t;

typedef struct {
    fsm_state_t     state;
    ymodem_ctx_t    *ctx;
    uint32_t        flash_addr;         /* base download address */
    uint8_t         buf[FRAME_BUF_SIZE];
    uint16_t        buf_len;
    uint16_t        buf_idx;
    uint8_t         frame_type;
    uint16_t        retry;
    uint32_t        timer_start;
    bool            is_end_frame;
    uint16_t        total_packets;
} ymodem_fsm_t;

/*---------------------------------------------------------------------------
 * Module static variable
 *---------------------------------------------------------------------------*/
static ymodem_fsm_t fsm;

/*---------------------------------------------------------------------------
 * Internal function declarations
 *---------------------------------------------------------------------------*/
static void fsm_dispatch(void);
static void set_timeout(uint32_t ms);
static bool is_timeout(uint32_t ms);
static void send_byte(uint8_t b);
static int32_t read_byte_timeout(uint32_t ms);
static void send_ack(void);
static void send_nak(void);
static void cancel_transfer(void);

static void state_init(void);
static void state_wait_file_info(void);
static void state_data_phase(void);
static void state_rx_frame(void);
static void state_eot_phase(void);

static uint32_t calc_frame_crc(const uint8_t *data, uint16_t len);
static int parse_file_info(const uint8_t *data, ymodem_ctx_t *ctx);

/*---------------------------------------------------------------------------
 * Public interface
 *---------------------------------------------------------------------------*/
ymodem_status_t ymodem_receive(ymodem_ctx_t *ctx, uint32_t flash_addr) {
    printf("[Ymodem] === Ymodem receive start ===\r\n");

    memset(&fsm, 0, sizeof(fsm));
    fsm.ctx         = ctx;
    fsm.flash_addr  = flash_addr;
    fsm.state       = STATE_INIT;
    fsm.is_end_frame = false;

    ctx->received_size = 0;
    ctx->packet_seq    = 1;
    ctx->write_addr    = flash_addr;    /* start of Download area */
    crc32_init(&ctx->current_crc);

    while (fsm.state != STATE_COMPLETE && fsm.state != STATE_ERROR) {
        fsm_dispatch();
        ymodem_feed_watchdog();
    }

    if (fsm.state == STATE_COMPLETE) {
        uint32_t final_crc = crc32_finalize(&ctx->current_crc);
        printf("[Ymodem] Transfer complete. File CRC: 0x%08X, Expected: 0x%08X\r\n",
               final_crc, ctx->file_crc);
        if (final_crc != ctx->file_crc) {
            printf("[Ymodem] ERROR: File CRC mismatch!\r\n");
            return YMODEM_ERR_CRC;
        }
        printf("[Ymodem] File CRC OK.\r\n");
        return YMODEM_OK;
    }

    printf("[Ymodem] Transfer failed, state: %d\r\n", fsm.state);
    return YMODEM_ERR_TIMEOUT;
}

/*---------------------------------------------------------------------------
 * State dispatcher
 *---------------------------------------------------------------------------*/
static void fsm_dispatch(void) {
    switch (fsm.state) {
        case STATE_INIT:           state_init();            break;
        case STATE_WAIT_FILE_INFO: state_wait_file_info();  break;
        case STATE_DATA_PHASE:     state_data_phase();      break;
        case STATE_RX_FRAME:       state_rx_frame();        break;
        case STATE_EOT_PHASE:      state_eot_phase();       break;
        case STATE_COMPLETE:
        case STATE_ERROR:
            break;
        default:
            printf("[Ymodem] ERROR: Illegal state %d\r\n", fsm.state);
            fsm.state = STATE_ERROR;
            break;
    }
}

/*---------------------------------------------------------------------------
 * Timeout helpers
 *---------------------------------------------------------------------------*/
static void set_timeout(uint32_t ms) {
    (void)ms;
    fsm.timer_start = ymodem_get_tick();
}

static bool is_timeout(uint32_t ms) {
    return (ymodem_get_tick() - fsm.timer_start) >= ms;
}

/*---------------------------------------------------------------------------
 * Low-level I/O
 *---------------------------------------------------------------------------*/
static void send_byte(uint8_t b) {
    ymodem_send_byte(b);
}

static int32_t read_byte_timeout(uint32_t ms) {
    return ymodem_read_byte(ms);
}

static void send_ack(void) {
    send_byte(YMODEM_ACK);
}

static void send_nak(void) {
    printf("[Ymodem] -> NAK\r\n");
    send_byte(YMODEM_NAK);
}

static void cancel_transfer(void) {
    printf("[Ymodem] -> CAN (cancel transfer)\r\n");
    for (int i = 0; i < 5; i++) {
        send_byte(YMODEM_CAN);
    }
    fsm.state = STATE_ERROR;
}

/*---------------------------------------------------------------------------
 * State handlers
 *---------------------------------------------------------------------------*/
static void state_init(void) {
    printf("[Ymodem] Sending 'C' to initiate handshake...\r\n");
    for (int i = 0; i < 5; i++) {
        send_byte(YMODEM_C);
        for (volatile int d = 0; d < 1000; d++);
    }
    fsm.retry = 0;
    set_timeout(FILE_INFO_TIMEOUT);
    fsm.state = STATE_WAIT_FILE_INFO;
}

static void state_wait_file_info(void) {
    int32_t ch = read_byte_timeout(POLL_TIMEOUT_MS);
    if (ch < 0) {
        if (is_timeout(FILE_INFO_TIMEOUT)) {
            if (++fsm.retry > YMODEM_MAX_RETRY) {
                printf("[Ymodem] ERROR: File info timeout, cancelling\r\n");
                cancel_transfer();
            } else {
                printf("[Ymodem] File info timeout, resending 'C' (retry %d)\r\n", fsm.retry);
                fsm.state = STATE_INIT;
            }
        }
        return;
    }

    if (ch == YMODEM_EOT) {
        printf("[Ymodem] Received EOT (empty file)\r\n");
        send_ack();
        fsm.state = STATE_COMPLETE;
    } else if (ch == YMODEM_SOH || ch == YMODEM_STX) {
        printf("[Ymodem] File info frame header: 0x%02X\r\n", ch);
        fsm.frame_type = (uint8_t)ch;
        fsm.buf[0]     = fsm.frame_type;
        fsm.buf_idx    = 1;
        fsm.buf_len    = 3 + YMODEM_FILE_INFO_SIZE + 4;
        set_timeout(FILE_INFO_TIMEOUT);
        fsm.state      = STATE_RX_FRAME;
        fsm.retry      = 0;
    } else {
        printf("[Ymodem] Unexpected char 0x%02X, sending NAK\r\n", ch);
        send_nak();
    }
}

static void state_data_phase(void) {
    int32_t ch = read_byte_timeout(POLL_TIMEOUT_MS);
    if (ch < 0) {
        if (is_timeout(DATA_TIMEOUT)) {
            if (++fsm.retry > YMODEM_MAX_RETRY) {
                printf("[Ymodem] ERROR: Data phase timeout, cancelling\r\n");
                cancel_transfer();
            } else {
                printf("[Ymodem] Data phase timeout, sending NAK (retry %d)\r\n", fsm.retry);
                send_nak();
                set_timeout(DATA_TIMEOUT);
            }
        }
        return;
    }

    if (ch == YMODEM_EOT) {
        printf("[Ymodem] Received EOT, entering end phase\r\n");
        send_nak();
        set_timeout(EOT_TIMEOUT);
        fsm.state = STATE_EOT_PHASE;
        fsm.retry = 0;
    } else if (ch == YMODEM_STX || ch == YMODEM_SOH) {
        uint16_t data_len = (ch == YMODEM_STX) ? YMODEM_PACKET_SIZE : 128;
        printf("[Ymodem] Data frame header: 0x%02X, data length: %d\r\n", ch, data_len);
        fsm.frame_type = (uint8_t)ch;
        fsm.buf[0]     = fsm.frame_type;
        fsm.buf_idx    = 1;
        fsm.buf_len    = 3 + data_len + 4;
        set_timeout(DATA_TIMEOUT);
        fsm.state = STATE_RX_FRAME;
        fsm.retry = 0;
    } else {
        printf("[Ymodem] Unexpected char 0x%02X, sending NAK\r\n", ch);
        send_nak();
    }
}

static void state_rx_frame(void) {
    while (fsm.buf_idx < fsm.buf_len) {
        int32_t b = read_byte_timeout(POLL_TIMEOUT_MS);
        if (b < 0) {
            if (is_timeout(DATA_TIMEOUT)) {
                printf("[Ymodem] Frame recv timeout (got %d/%d bytes)\r\n", fsm.buf_idx, fsm.buf_len);
                send_nak();
                fsm.retry++;
                fsm.state = (fsm.frame_type == YMODEM_SOH && 
                             fsm.buf_len == (3 + YMODEM_FILE_INFO_SIZE + 4)) ?
                            STATE_WAIT_FILE_INFO : STATE_DATA_PHASE;
                return;
            }
            continue;
        }
        fsm.buf[fsm.buf_idx++] = (uint8_t)b;
        set_timeout(DATA_TIMEOUT);
    }

    uint8_t  seq      = fsm.buf[1];
    uint8_t  seq_comp = fsm.buf[2];
    uint16_t data_len = (fsm.frame_type == YMODEM_STX) ? YMODEM_PACKET_SIZE : YMODEM_FILE_INFO_SIZE;
    uint8_t *data     = fsm.buf + 3;
    uint32_t rcv_crc  = *((uint32_t *)(data + data_len));
    uint32_t calc_crc = calc_frame_crc(data, data_len);

    printf("[Ymodem] Frame received: type=%c, seq=%d, data_len=%d, calcCRC=0x%08X, recvCRC=0x%08X\r\n",
           fsm.frame_type == YMODEM_STX ? 'T' : 'S', seq, data_len, calc_crc, rcv_crc);

    if ((uint8_t)(seq + seq_comp) != 0xFF) {
        printf("[Ymodem] Sequence error: seq=%d, ~seq=%d (expected 0xFF)\r\n", seq, seq_comp);
        send_nak();
        fsm.state = (fsm.buf_len == (3 + YMODEM_FILE_INFO_SIZE + 4)) ?
                    STATE_WAIT_FILE_INFO : STATE_DATA_PHASE;
        return;
    }

    if (calc_crc != rcv_crc) {
        printf("[Ymodem] Frame CRC error!\r\n");
        send_nak();
        fsm.state = (fsm.buf_len == (3 + YMODEM_FILE_INFO_SIZE + 4)) ?
                    STATE_WAIT_FILE_INFO : STATE_DATA_PHASE;
        return;
    }

    if (fsm.frame_type == YMODEM_SOH && seq == 0 && data_len == YMODEM_FILE_INFO_SIZE) {
        if (fsm.is_end_frame) {
            printf("[Ymodem] End frame received\r\n");
            send_ack();
            fsm.state = STATE_COMPLETE;
            fsm.is_end_frame = false;
        } else {
            printf("[Ymodem] Parsing file info...\r\n");
            if (parse_file_info(data, fsm.ctx) != 0) {
                printf("[Ymodem] File info parse failed, cancelling\r\n");
                cancel_transfer();
                return;
            }
            printf("[Ymodem] File: %s, Size: %lu, Expected CRC: 0x%08X\r\n",
                   fsm.ctx->file_name, fsm.ctx->file_size, fsm.ctx->file_crc);
            fsm.total_packets = (fsm.ctx->file_size + YMODEM_PACKET_SIZE - 1) / YMODEM_PACKET_SIZE;
            fsm.ctx->packet_seq = 1;
            crc32_init(&fsm.ctx->current_crc);
            fsm.ctx->received_size = 0;
            printf("[Ymodem] -> ACK + 'C'\r\n");
            send_ack();
            send_byte(YMODEM_C);
            set_timeout(DATA_TIMEOUT);
            fsm.state = STATE_DATA_PHASE;
        }
    } else {
        /* Data frame */
        if (seq == fsm.ctx->packet_seq) {
            uint16_t valid_len = data_len;
            if (fsm.ctx->received_size + valid_len > fsm.ctx->file_size) {
                valid_len = fsm.ctx->file_size - fsm.ctx->received_size;
            }

            /* ---- Write valid data to flash ---- */
            if (!flash_write(fsm.ctx->write_addr, data, valid_len)) {
                /* 读取写入地址的第一个字，打印出来 */
                uint32_t dbg_word = *(volatile uint32_t *)fsm.ctx->write_addr;
                printf("[Ymodem] Flash write error at 0x%08X, current value: 0x%08X\r\n",
                    (unsigned)fsm.ctx->write_addr, (unsigned)dbg_word);
                cancel_transfer();
                return;
            }
            fsm.ctx->write_addr += valid_len;

            crc32_update(&fsm.ctx->current_crc, data, valid_len);
            fsm.ctx->received_size += valid_len;
            printf("[Ymodem] Data frame #%d OK, accumulated %lu/%lu bytes (%d%%)\r\n",
                   seq, fsm.ctx->received_size, fsm.ctx->file_size,
                   (int)(fsm.ctx->received_size * 100 / fsm.ctx->file_size));
            fsm.ctx->packet_seq++;
            send_ack();
        } else if (seq == (uint8_t)(fsm.ctx->packet_seq - 1)) {
            printf("[Ymodem] Duplicate frame #%d, ACK but ignore data\r\n", seq);
            send_ack();
        } else {
            printf("[Ymodem] Seq mismatch: expected %d, received %d\r\n", fsm.ctx->packet_seq, seq);
            send_nak();
        }
        fsm.state = STATE_DATA_PHASE;
        set_timeout(DATA_TIMEOUT);
    }
}

static void state_eot_phase(void) {
    int32_t ch = read_byte_timeout(POLL_TIMEOUT_MS);
    if (ch < 0) {
        if (is_timeout(EOT_TIMEOUT)) {
            printf("[Ymodem] EOT phase timeout, cancelling\r\n");
            cancel_transfer();
        }
        return;
    }

    if (ch == YMODEM_EOT) {
        printf("[Ymodem] Received second EOT\r\n");
        send_ack();
        send_byte(YMODEM_C);
        printf("[Ymodem] -> ACK + 'C', waiting for end frame\r\n");
        fsm.is_end_frame = true;
        fsm.frame_type = YMODEM_SOH;
        fsm.buf_len = 3 + YMODEM_FILE_INFO_SIZE + 4;
        fsm.buf_idx = 0;
        set_timeout(EOT_TIMEOUT);
        fsm.state = STATE_RX_FRAME;
        fsm.retry = 0;
    } else if (ch == YMODEM_SOH) {
        printf("[Ymodem] Directly received end frame\r\n");
        fsm.is_end_frame = true;
        fsm.buf[0] = YMODEM_SOH;
        fsm.buf_idx = 1;
        fsm.buf_len = 3 + YMODEM_FILE_INFO_SIZE + 4;
        set_timeout(EOT_TIMEOUT);
        fsm.state = STATE_RX_FRAME;
    } else {
        printf("[Ymodem] EOT phase unexpected char 0x%02X\r\n", ch);
        send_nak();
    }
}

/*---------------------------------------------------------------------------
 * CRC and parsing helpers
 *---------------------------------------------------------------------------*/
static uint32_t calc_frame_crc(const uint8_t *data, uint16_t len) {
    uint32_t crc;
    crc32_init(&crc);
    crc32_update(&crc, data, len);
    return crc32_finalize(&crc);
}

static int parse_file_info(const uint8_t *data, ymodem_ctx_t *ctx) {
    int i = 0;
    while (i < YMODEM_FILE_INFO_SIZE && data[i] != '\0') {
        ctx->file_name[i] = (char)data[i];
        i++;
    }
    if (i >= YMODEM_FILE_INFO_SIZE || i == 0) {
        printf("[Ymodem] Parse fail: invalid filename\r\n");
        return -1;
    }
    ctx->file_name[i] = '\0';
    i++;

    while (i < YMODEM_FILE_INFO_SIZE && data[i] == ' ') i++;

    char size_str[16] = {0};
    int j = 0;
    while (i < YMODEM_FILE_INFO_SIZE && data[i] != ' ' && data[i] != '\0' && j < 15) {
        size_str[j++] = (char)data[i++];
    }
    ctx->file_size = strtoul(size_str, NULL, 16);

    while (i < YMODEM_FILE_INFO_SIZE && data[i] == ' ') i++;

    char crc_str[16] = {0};
    j = 0;
    while (i < YMODEM_FILE_INFO_SIZE && data[i] != ' ' && data[i] != '\0' && j < 15) {
        crc_str[j++] = (char)data[i++];
    }
    ctx->file_crc = strtoul(crc_str, NULL, 16);

    return 0;
}

