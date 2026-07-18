#include "var_manager.h"
#include "app_config.h"
#include <string.h>
#include "data_link.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "protocol.h"
#include "logger.h"

/* ---------- 内部变量 ---------- */
static SemaphoreHandle_t var_mutex;
static VarEntry registry[HOSTLINK_MAX_VARS];
static uint8_t reg_count = 0;
static uint16_t subscribed[HOSTLINK_MAX_SUBSCRIBE];
static uint8_t sub_count = 0;

/* ---------- 初始化 ---------- */
void VAR_Init(void)
{
    var_mutex = xSemaphoreCreateMutex();
    reg_count = 0;
    sub_count = 0;
}

/* ---------- 注册变量 ---------- */
int VAR_Register(uint16_t id, const char *name, VarType type, uint8_t perm, void *ptr)
{
    if (xSemaphoreTake(var_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        LOG_Printf("VAR_Register timeout\r\n");
        return -1;
    }

    if (reg_count >= HOSTLINK_MAX_VARS) {
        xSemaphoreGive(var_mutex);
        return -2;
    }

    registry[reg_count].id = id;
    registry[reg_count].name = name;
    registry[reg_count].type = type;
    registry[reg_count].permission = perm;
    registry[reg_count].ptr = ptr;
    reg_count++;

    xSemaphoreGive(var_mutex);
    return 0;
}

/* ---------- 读取变量 ---------- */
int VAR_Read(uint16_t id, void *buf, uint16_t *len)
{
    if (xSemaphoreTake(var_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        LOG_Printf("VAR_Read timeout\r\n");
        return -1;
    }

    int ret = -1;
    for (int i = 0; i < reg_count; i++) {
        if (registry[i].id == id) {
            switch (registry[i].type) {
                case VAR_TYPE_UINT8:
                    *(uint8_t*)buf = *(uint8_t*)registry[i].ptr;
                    *len = 1;
                    ret = 0;
                    break;
                case VAR_TYPE_INT16:
                    *(int16_t*)buf = *(int16_t*)registry[i].ptr;
                    *len = 2;
                    ret = 0;
                    break;
                case VAR_TYPE_INT32:
                    *(int32_t*)buf = *(int32_t*)registry[i].ptr;
                    *len = 4;
                    ret = 0;
                    break;
                case VAR_TYPE_FLOAT:
                    *(float*)buf = *(float*)registry[i].ptr;
                    *len = 4;
                    ret = 0;
                    break;
                default:
                    ret = -2;
                    break;
            }
            break;
        }
    }

    xSemaphoreGive(var_mutex);
    return ret;
}

/* ---------- 写入变量 ---------- */
int VAR_Write(uint16_t id, const void *buf, uint16_t len)
{
    if (xSemaphoreTake(var_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        LOG_Printf("VAR_Write timeout\r\n");
        return -1;
    }

    int ret = -1;
    for (int i = 0; i < reg_count; i++) {
        if (registry[i].id == id && registry[i].permission == 1) {
            uint16_t size = 0;
            switch (registry[i].type) {
                case VAR_TYPE_UINT8:  size = 1; break;
                case VAR_TYPE_INT16:  size = 2; break;
                case VAR_TYPE_INT32:  size = 4; break;
                case VAR_TYPE_FLOAT:  size = 4; break;
                default: break;
            }
            if (size > 0) {
                memcpy(registry[i].ptr, buf, len < size ? len : size);
                ret = 0;
            }
            break;
        }
    }

    xSemaphoreGive(var_mutex);
    return ret;
}

/* ---------- 订阅变量 ---------- */
void VAR_Subscribe(uint16_t id)
{
    if (xSemaphoreTake(var_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        LOG_Printf("VAR_Subscribe timeout\r\n");
        return;
    }

    if (sub_count >= HOSTLINK_MAX_SUBSCRIBE) {
        xSemaphoreGive(var_mutex);
        return;
    }

    for (int i = 0; i < sub_count; i++) {
        if (subscribed[i] == id) {
            xSemaphoreGive(var_mutex);   // 已修复：提前返回必须释放锁
            return;
        }
    }

    subscribed[sub_count++] = id;
    xSemaphoreGive(var_mutex);
}

/* ---------- 清空订阅 ---------- */
void VAR_ClearSubscriptions(void)
{
    if (xSemaphoreTake(var_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        LOG_Printf("VAR_ClearSubscriptions timeout\r\n");
        return;
    }
    sub_count = 0;
    xSemaphoreGive(var_mutex);
}

/* ---------- 发送变量列表（支持分片，当前只发第一包） ---------- */
void VAR_SendList(void)
{
    if (xSemaphoreTake(var_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        LOG_Printf("VAR_SendList timeout\r\n");
        return;
    }

    const uint16_t max_payload = HOSTLINK_TX_DMA_CHUNK - 10; // 帧头+CRC等约需10字节
    uint8_t packet_index = 0;
    uint16_t sent_count = 0;            // 已发送变量数量
    uint8_t buf[HOSTLINK_TX_DMA_CHUNK];

    while (sent_count < reg_count) {
        buf[0] = SYNC1;
        buf[1] = SYNC2;
        buf[2] = CMD_LIST_VARS;
        uint16_t idx = 7;               // 跳过分片字段

        int count_in_packet = 0;
        for (int i = sent_count; i < reg_count; i++) {
            uint8_t name_len = strlen(registry[i].name);
            if (idx + 5 + name_len > max_payload) break;  // 防止溢出

            buf[idx++] = registry[i].id & 0xFF;
            buf[idx++] = (registry[i].id >> 8) & 0xFF;
            buf[idx++] = registry[i].type;
            buf[idx++] = registry[i].permission;
            buf[idx++] = name_len;
            memcpy(&buf[idx], registry[i].name, name_len);
            idx += name_len;
            sent_count++;
            count_in_packet++;
        }

        // 分片信息 (total_packets, packet_index)
        int total = (reg_count + count_in_packet - 1) / count_in_packet; // 粗略估算
        buf[5] = (uint8_t)total;
        buf[6] = packet_index++;

        uint16_t payload_len = idx - 7;
        buf[3] = payload_len & 0xFF;
        buf[4] = (payload_len >> 8) & 0xFF;

        DataLink_SendPacket(buf, idx);
        // 当前退化为只发第一包，若需多包则注释掉 break
        break;
    }

    xSemaphoreGive(var_mutex);
}

/* ---------- 获取当前订阅列表 ---------- */
void VAR_GetSubscribedList(uint16_t *ids, uint8_t *count)
{
    if (xSemaphoreTake(var_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        LOG_Printf("VAR_GetSubscribedList timeout\r\n");
        *count = 0;
        return;
    }

    memcpy(ids, subscribed, sub_count * sizeof(uint16_t));
    *count = sub_count;

    xSemaphoreGive(var_mutex);
}