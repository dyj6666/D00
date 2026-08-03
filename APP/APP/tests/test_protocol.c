/* 主机单元测试：协议帧构造/校验/解析 + LIST_VARS 分片打包 */
#include <stdio.h>
#include <string.h>
#include "protocol.h"
#include "crc16.h"
#include "var_list.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

static void test_crc16_known_vector(void)
{
    const char *data = "123456789";
    uint16_t crc = CRC16_Calculate((const uint8_t *)data, 9);
    CHECK(crc == 0x4B37, "CRC-16/MODBUS 标准校验向量 0x4B37");
}

static void test_build_and_validate_frame(void)
{
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t frame[64];
    uint16_t len = 0;

    CHECK(Protocol_BuildFrame(frame, sizeof(frame), CMD_WRITE_VAR,
                              payload, sizeof(payload), &len) == PROTO_ERR_NONE,
          "BuildFrame 成功");
    CHECK(len == PROTOCOL_HEADER_LEN + sizeof(payload) + PROTOCOL_CRC_LEN,
          "BuildFrame 长度正确");
    CHECK(Protocol_ValidateFrame(frame, len) == PROTO_ERR_NONE,
          "ValidateFrame 通过");
    CHECK(frame[0] == SYNC1 && frame[1] == SYNC2, "帧头同步字正确");
    CHECK(frame[2] == CMD_WRITE_VAR, "命令码正确");

    /* 篡改一个 payload 字节 -> CRC 失败 */
    frame[6] ^= 0xFF;
    CHECK(Protocol_ValidateFrame(frame, len) == PROTO_ERR_CRC,
          "篡改数据被 CRC 拦截");
    frame[6] ^= 0xFF;

    /* 破坏同步字 */
    frame[0] = 0x00;
    CHECK(Protocol_ValidateFrame(frame, len) == PROTO_ERR_BAD_SYNC,
          "坏同步字被识别");
    frame[0] = SYNC1;

    /* 太短 */
    CHECK(Protocol_ValidateFrame(frame, PROTOCOL_HEADER_LEN + 1) == PROTO_ERR_TOO_SHORT,
          "过短帧被拒绝");
}

static void test_parse_header(void)
{
    const uint8_t payload[] = {0xAA, 0xBB};
    uint8_t frame[32];
    uint16_t len = 0;
    Protocol_BuildFrame(frame, sizeof(frame), CMD_READ_VAR,
                        payload, sizeof(payload), &len);

    /* 不含 CRC 的帧数据 */
    protocol_frame_t f;
    CHECK(Protocol_ParseHeader(frame, len - PROTOCOL_CRC_LEN, &f) == PROTO_ERR_NONE,
          "ParseHeader 成功");
    CHECK(f.cmd == CMD_READ_VAR && f.payload_len == 2 &&
          f.payload != NULL && f.payload[0] == 0xAA && f.payload[1] == 0xBB,
          "ParseHeader 字段正确");

    /* payload_len 声明与实际不符 -> 拒绝 */
    frame[3] = 99;
    CHECK(Protocol_ParseHeader(frame, len - PROTOCOL_CRC_LEN, &f) == PROTO_ERR_BAD_PAYLOAD_LEN,
          "payload_len 不一致被拒绝");
    frame[3] = 2;

    /* 最小帧（零 payload） */
    uint8_t empty[PROTOCOL_HEADER_LEN + PROTOCOL_CRC_LEN];
    uint16_t elen = 0;
    Protocol_BuildFrame(empty, sizeof(empty), CMD_LIST_VARS, NULL, 0, &elen);
    CHECK(elen == PROTOCOL_HEADER_LEN + PROTOCOL_CRC_LEN, "零 payload 帧长度正确");
    CHECK(Protocol_ParseHeader(empty, PROTOCOL_HEADER_LEN, &f) == PROTO_ERR_NONE &&
          f.payload_len == 0, "零 payload 解析正确");
}

static void test_build_frame_bounds(void)
{
    const uint8_t payload[] = {1, 2, 3};
    uint8_t tiny[6];
    uint16_t len = 0;
    CHECK(Protocol_BuildFrame(tiny, sizeof(tiny), CMD_DATA, payload, 3, &len)
              == PROTO_ERR_BUF_TOO_SMALL,
          "缓冲不足返回 BUF_TOO_SMALL");
}

static void test_var_list_packets(void)
{
    static const char *names[] = {
        "sys_tick", "led_state", "key_count", "ota_status", "la_samples",
        "la_ch0", "la_ch4", "temp", "voltage", "current",
        "pwm_duty", "adc_ch0", "adc_ch1", "adc_ch2", "angle",
        "speed", "accel_x", "accel_y", "accel_z", "gyro_z"
    };
    VarEntry entries[20];
    for (int i = 0; i < 20; i++) {
        entries[i].id = (uint16_t)(0x1000 + i);
        entries[i].name = names[i];
        entries[i].type = (VarType)(i % 4);
        entries[i].permission = (uint8_t)(i % 2);
        entries[i].ptr = NULL;
    }

    const uint16_t max_frame = 126;   /* HOSTLINK_TX_DMA_CHUNK - CRC */
    uint8_t total = VarList_TotalPackets(entries, 20, max_frame);
    CHECK(total > 1, "20 个变量产生多包");

    uint16_t seen_ids[20];
    uint16_t seen_count = 0;
    for (uint8_t pkt = 0; pkt < total; pkt++) {
        uint8_t frame[160];
        uint16_t len = 0;
        CHECK(VarList_BuildPacket(entries, 20, max_frame, total, pkt,
                                  frame, sizeof(frame), &len) == 0,
              "BuildPacket 成功");
        CHECK(len <= max_frame, "单包不超过帧上限");

        /* 附加 CRC 后整帧可校验 */
        uint16_t crc = CRC16_Calculate(frame, len);
        frame[len] = (uint8_t)(crc & 0xFF);
        frame[len + 1] = (uint8_t)((crc >> 8) & 0xFF);
        CHECK(Protocol_ValidateFrame(frame, len + 2) == PROTO_ERR_NONE,
              "LIST_VARS 分片帧 CRC 合法");
        CHECK(frame[5] == total, "total_packets 字段正确");
        CHECK(frame[6] == pkt, "packet_index 字段正确");
        CHECK(((frame[3] | (frame[4] << 8)) & 0xFFFF) == (len - 5),
              "payload_len 字段正确");

        /* 解析本包条目 */
        uint16_t off = 7;
        while (off < len) {
            uint16_t id = (uint16_t)(frame[off] | (frame[off + 1] << 8));
            uint8_t name_len = frame[off + 4];
            off += 5 + name_len;
            CHECK(off <= len, "条目不越界");
            seen_ids[seen_count++] = id;
        }
    }
    CHECK(seen_count == 20, "所有变量恰好出现一次");
    for (int i = 0; i < 20; i++) {
        int found = 0;
        for (int j = 0; j < seen_count; j++) {
            if (seen_ids[j] == (uint16_t)(0x1000 + i)) { found = 1; break; }
        }
        CHECK(found, "变量 ID 无遗漏");
    }
}

static void test_var_list_empty(void)
{
    uint8_t total = VarList_TotalPackets(NULL, 0, 126);
    CHECK(total == 1, "空表产生 1 个空包");
    uint8_t frame[32];
    uint16_t len = 0;
    CHECK(VarList_BuildPacket(NULL, 0, 126, total, 0, frame, sizeof(frame), &len) == 0,
          "空包构建成功");
    CHECK(len == 7 && frame[5] == 1 && frame[6] == 0,
          "空包仅含分片头");
}

int main(void)
{
    printf("== 主机单元测试 ==\n");
    test_crc16_known_vector();
    test_build_and_validate_frame();
    test_parse_header();
    test_build_frame_bounds();
    test_var_list_packets();
    test_var_list_empty();

    if (failures == 0) {
        printf("\nALL TESTS PASSED\n");
        return 0;
    }
    printf("\n%d TEST(S) FAILED\n", failures);
    return 1;
}
