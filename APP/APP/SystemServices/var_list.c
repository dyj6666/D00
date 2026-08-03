#include "var_list.h"
#include "protocol.h"
#include <string.h>

#define VAR_LIST_FRAG_LEN 2   /* total_packets + packet_index */

/* 条目编码后占用字节数 */
static uint16_t entry_encoded_len(const VarEntry *e)
{
    return (uint16_t)(5 + strlen(e->name));
}

/* 从 start 起，最多能装进 (budget) 字节的条目数 */
static uint16_t count_fit(const VarEntry *entries, uint16_t count,
                          uint16_t start, uint16_t budget)
{
    uint16_t used = 0;
    uint16_t n = 0;
    for (uint16_t i = start; i < count; i++) {
        uint16_t e = entry_encoded_len(&entries[i]);
        if ((uint32_t)used + e > budget) break;
        used += e;
        n++;
    }
    return n;
}

uint8_t VarList_TotalPackets(const VarEntry *entries, uint16_t count,
                             uint16_t max_frame_len)
{
    uint16_t budget = (max_frame_len > 7) ? (max_frame_len - 7) : 0;
    if (budget == 0) return 1;

    uint16_t idx = 0;
    uint16_t total = 0;
    while (idx < count) {
        uint16_t n = count_fit(entries, count, idx, budget);
        if (n == 0) n = 1;   /* 单条超长：仍占一包，保证进度 */
        idx += n;
        total++;
        if (total >= 255) return 255;
    }
    if (total == 0) total = 1;   /* 空表也发一包 */
    return (uint8_t)total;
}

int VarList_BuildPacket(const VarEntry *entries, uint16_t count,
                        uint16_t max_frame_len,
                        uint8_t total_packets, uint8_t packet_index,
                        uint8_t *frame, uint16_t cap, uint16_t *out_len)
{
    if (frame == NULL || out_len == NULL) return -1;

    uint16_t budget = (max_frame_len > 7) ? (max_frame_len - 7) : 0;
    if (budget == 0) return -1;

    /* 定位本包起始条目 */
    uint16_t start = 0;
    for (uint8_t p = 0; p < packet_index; p++) {
        uint16_t n = count_fit(entries, count, start, budget);
        if (n == 0) n = 1;
        start += n;
        if (start >= count) break;
    }

    /* 第一遍：统计本包可装条目数（不写缓冲，避免容量不足时越界） */
    uint16_t used = 0;
    uint16_t i = start;
    while (i < count) {
        uint16_t e = entry_encoded_len(&entries[i]);
        if ((uint32_t)used + e > budget) break;
        used += e;
        i++;
    }

    uint16_t total_len = (uint16_t)(7 + used);
    if (total_len > cap) return -2;

    /* 第二遍：实际填包 */
    used = 0;
    i = start;
    while (i < count) {
        uint16_t e = entry_encoded_len(&entries[i]);
        if ((uint32_t)used + e > budget) break;
        const uint8_t *name = (const uint8_t *)entries[i].name;
        uint8_t name_len = (uint8_t)strlen(entries[i].name);

        frame[7 + used + 0] = entries[i].id & 0xFF;
        frame[7 + used + 1] = (entries[i].id >> 8) & 0xFF;
        frame[7 + used + 2] = (uint8_t)entries[i].type;
        frame[7 + used + 3] = entries[i].permission;
        frame[7 + used + 4] = name_len;
        memcpy(&frame[7 + used + 5], name, name_len);
        used += e;
        i++;
    }

    frame[0] = SYNC1;
    frame[1] = SYNC2;
    frame[2] = CMD_LIST_VARS;
    uint16_t payload_len = VAR_LIST_FRAG_LEN + used;
    frame[3] = (uint8_t)(payload_len & 0xFF);
    frame[4] = (uint8_t)((payload_len >> 8) & 0xFF);
    frame[5] = total_packets;
    frame[6] = packet_index;

    *out_len = total_len;
    return 0;
}
