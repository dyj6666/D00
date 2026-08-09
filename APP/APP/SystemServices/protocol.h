#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* ================================================================
 * 上位机通信协议（HOSTLINK / DataLink）
 *
 * 帧格式（所有多字节字段均为小端）:
 *   [0]     SYNC1     0xAA
 *   [1]     SYNC2     0x55
 *   [2]     CMD       命令码
 *   [3..4]  payload_len (uint16 LE, 不含帧头与 CRC)
 *   [5..]   payload
 *   [last 2] CRC16-LE  (对帧头+payload 全部字节计算)
 *
 * 本文件为纯逻辑层，不依赖任何硬件，可在主机上做单元测试。
 * ================================================================ */

#define SYNC1                0xAA
#define SYNC2                0x55

#define PROTOCOL_HEADER_LEN  5       /* SYNC1+SYNC2+CMD+payload_len */
#define PROTOCOL_CRC_LEN     2
#define PROTOCOL_VERSION     1

/* ---------------- 命令码 ---------------- */
#define CMD_LIST_VARS       0x01
#define CMD_SUBSCRIBE       0x02
#define CMD_DATA            0x03
#define CMD_READ_VAR        0x04
#define CMD_WRITE_VAR       0x05
#define CMD_GET_INFO        0x06
#define CMD_LA_DUMP         0x07
#define CMD_OTA_BEGIN       0x08
#define CMD_OTA_DATA        0x09
#define CMD_OTA_END         0x0A
#define CMD_OTA_STATUS      0x0B
#define CMD_OTA_RESET       0x0D   /* 强制复位 OTA 会话（清下载会话槽） */
#define CMD_ERROR           0xFE

/* ---------------- 错误码 ---------------- */
typedef enum {
    PROTO_ERR_NONE = 0,
    PROTO_ERR_BAD_SYNC,          /* 帧头同步字错误 */
    PROTO_ERR_TOO_SHORT,         /* 长度不足最小帧 */
    PROTO_ERR_CRC,               /* CRC 校验失败 */
    PROTO_ERR_BAD_PAYLOAD_LEN,   /* payload_len 字段与实际长度不符/非法 */
    PROTO_ERR_UNKNOWN_CMD,       /* 未知命令 */
    PROTO_ERR_VAR_NOT_FOUND,     /* 变量不存在 */
    PROTO_ERR_VAR_READONLY,      /* 变量只读 */
    PROTO_ERR_BUF_TOO_SMALL,     /* 输出缓冲不足 */
    PROTO_ERR_NO_MEM,            /* 内存/资源不足 */
} proto_err_t;

/* 错误响应帧 payload 布局（CMD_ERROR）:
 *   [0]        原命令码
 *   [1]        错误码 (proto_err_t)
 *   [2..3]     保留，置 0
 */
#define PROTOCOL_ERR_PAYLOAD_LEN 4

/* ---------------- 解析结果 ---------------- */
typedef struct {
    uint8_t        cmd;          /* 命令码 */
    uint16_t       payload_len;  /* 实际 payload 长度（已校验） */
    const uint8_t *payload;      /* 指向 payload 起点 */
} protocol_frame_t;

/* ---------------- 接口 ---------------- */

/* 校验一帧完整数据（含 CRC）。
 * data/len 为串口收到的原始数据（含 2 字节 CRC）。
 * 返回 PROTO_ERR_NONE 或具体错误码。 */
int Protocol_ValidateFrame(const uint8_t *data, uint16_t len);

/* 解析帧头（不含 CRC 的数据），校验 payload_len 与实际长度一致。
 * data/len 为不含 CRC 的帧数据（len >= PROTOCOL_HEADER_LEN）。
 * 成功返回 PROTO_ERR_NONE，并通过 frame 返回命令与 payload 视图。 */
int Protocol_ParseHeader(const uint8_t *data, uint16_t len,
                         protocol_frame_t *frame);

/* 组装完整帧（含 CRC）到 dst。cap 为 dst 容量。
 * 成功返回 PROTO_ERR_NONE 并通过 out_len 返回整帧长度。
 * 容量不足返回 PROTO_ERR_BUF_TOO_SMALL。 */
int Protocol_BuildFrame(uint8_t *dst, uint16_t cap, uint8_t cmd,
                        const uint8_t *payload, uint16_t payload_len,
                        uint16_t *out_len);

/* 获取协议/固件版本。ver 至少 4 字节:
 *   ver[0] = 协议版本, ver[1..3] = 固件 major/minor/patch */
void Protocol_GetVersion(uint8_t ver[4]);

#endif
