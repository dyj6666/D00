/* 上位机通信协议：帧构造/解析/校验（纯逻辑，可主机测试） */
#include "protocol.h"
#include "crc16.h"
#include <stddef.h>

int Protocol_ValidateFrame(const uint8_t *data, uint16_t len)
{
    if (data == NULL) return PROTO_ERR_TOO_SHORT;
    if (len < PROTOCOL_HEADER_LEN + PROTOCOL_CRC_LEN) return PROTO_ERR_TOO_SHORT;
    if (data[0] != SYNC1 || data[1] != SYNC2) return PROTO_ERR_BAD_SYNC;

    uint16_t crc_received = (uint16_t)(data[len - 2] | (data[len - 1] << 8));
    uint16_t crc_calc = CRC16_Calculate(data, len - PROTOCOL_CRC_LEN);
    if (crc_received != crc_calc) return PROTO_ERR_CRC;

    return PROTO_ERR_NONE;
}

int Protocol_ParseHeader(const uint8_t *data, uint16_t len,
                         protocol_frame_t *frame)
{
    if (data == NULL || frame == NULL) return PROTO_ERR_TOO_SHORT;
    if (len < PROTOCOL_HEADER_LEN) return PROTO_ERR_TOO_SHORT;
    if (data[0] != SYNC1 || data[1] != SYNC2) return PROTO_ERR_BAD_SYNC;

    uint16_t declared = (uint16_t)(data[3] | (data[4] << 8));
    if (declared != (uint16_t)(len - PROTOCOL_HEADER_LEN)) {
        return PROTO_ERR_BAD_PAYLOAD_LEN;
    }

    frame->cmd = data[2];
    frame->payload_len = declared;
    frame->payload = (declared > 0) ? &data[PROTOCOL_HEADER_LEN] : NULL;
    return PROTO_ERR_NONE;
}

int Protocol_BuildFrame(uint8_t *dst, uint16_t cap, uint8_t cmd,
                        const uint8_t *payload, uint16_t payload_len,
                        uint16_t *out_len)
{
    uint32_t total = (uint32_t)PROTOCOL_HEADER_LEN + payload_len + PROTOCOL_CRC_LEN;

    if (dst == NULL || out_len == NULL) return PROTO_ERR_TOO_SHORT;
    if (payload_len > 0 && payload == NULL) return PROTO_ERR_BAD_PAYLOAD_LEN;
    if (total > cap) return PROTO_ERR_BUF_TOO_SMALL;

    dst[0] = SYNC1;
    dst[1] = SYNC2;
    dst[2] = cmd;
    dst[3] = (uint8_t)(payload_len & 0xFF);
    dst[4] = (uint8_t)((payload_len >> 8) & 0xFF);
    if (payload_len > 0) {
        for (uint16_t i = 0; i < payload_len; i++) {
            dst[PROTOCOL_HEADER_LEN + i] = payload[i];
        }
    }

    uint16_t crc = CRC16_Calculate(dst, PROTOCOL_HEADER_LEN + payload_len);
    dst[PROTOCOL_HEADER_LEN + payload_len] = (uint8_t)(crc & 0xFF);
    dst[PROTOCOL_HEADER_LEN + payload_len + 1] = (uint8_t)((crc >> 8) & 0xFF);

    *out_len = (uint16_t)total;
    return PROTO_ERR_NONE;
}

void Protocol_GetVersion(uint8_t ver[4])
{
    if (ver == NULL) return;
    ver[0] = PROTOCOL_VERSION;
    ver[1] = 1;   /* 固件 major */
    ver[2] = 0;   /* 固件 minor */
    ver[3] = 0;   /* 固件 patch */
}
