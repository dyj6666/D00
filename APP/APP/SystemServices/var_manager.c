#include "var_manager.h"
#include "app_config.h"
#include <string.h>
#include "data_link.h"   // 为了调用 DataLink_SendPacket
#include "FreeRTOS.h"
#include "semphr.h"
#include "protocol.h"

static SemaphoreHandle_t var_mutex;

static VarEntry registry[HOSTLINK_MAX_VARS];
static uint8_t reg_count = 0;

static uint16_t subscribed[HOSTLINK_MAX_SUBSCRIBE];
static uint8_t sub_count = 0;

void VAR_Init(void) {
    var_mutex = xSemaphoreCreateMutex();
    reg_count = 0;
    sub_count = 0;
}

int VAR_Register(uint16_t id, const char *name, VarType type, uint8_t perm, void *ptr) {
    xSemaphoreTake(var_mutex, portMAX_DELAY);
    if (reg_count >= HOSTLINK_MAX_VARS) return -1;
    registry[reg_count].id = id;
    registry[reg_count].name = name;
    registry[reg_count].type = type;
    registry[reg_count].permission = perm;
    registry[reg_count].ptr = ptr;
    reg_count++;
    xSemaphoreGive(var_mutex);
    return 0;
}

int VAR_Read(uint16_t id, void *buf, uint16_t *len) {
    xSemaphoreTake(var_mutex, portMAX_DELAY);
    for (int i = 0; i < reg_count; i++) {
        if (registry[i].id == id) {
            switch (registry[i].type) {
                case VAR_TYPE_UINT8: *(uint8_t*)buf = *(uint8_t*)registry[i].ptr; *len = 1; return 0;
                case VAR_TYPE_INT16: *(int16_t*)buf = *(int16_t*)registry[i].ptr; *len = 2; return 0;
                case VAR_TYPE_INT32: *(int32_t*)buf = *(int32_t*)registry[i].ptr; *len = 4; return 0;
                case VAR_TYPE_FLOAT: *(float*)buf = *(float*)registry[i].ptr; *len = 4; return 0;
                default: return -1;
            }
        }
    }
    xSemaphoreGive(var_mutex);
    return -1;
}

int VAR_Write(uint16_t id, const void *buf, uint16_t len) {
    xSemaphoreTake(var_mutex, portMAX_DELAY);
    for (int i = 0; i < reg_count; i++) {
        if (registry[i].id == id && registry[i].permission == 1) {
            uint16_t size = 0;
            switch (registry[i].type) {
                case VAR_TYPE_UINT8: size = 1; break;
                case VAR_TYPE_INT16: size = 2; break;
                case VAR_TYPE_INT32: size = 4; break;
                case VAR_TYPE_FLOAT: size = 4; break;
            }
            memcpy(registry[i].ptr, buf, len < size ? len : size);
            return 0;
        }
    }
    xSemaphoreGive(var_mutex);
    return -1;
}

void VAR_Subscribe(uint16_t id) {
    xSemaphoreTake(var_mutex, portMAX_DELAY);
    if (sub_count >= HOSTLINK_MAX_SUBSCRIBE) return;
    subscribed[sub_count++] = id;
    xSemaphoreGive(var_mutex);
}

void VAR_ClearSubscriptions(void) {
    xSemaphoreTake(var_mutex, portMAX_DELAY);
    sub_count = 0;
    xSemaphoreGive(var_mutex);
}

void VAR_SendList(void) {
    xSemaphoreTake(var_mutex, portMAX_DELAY);
    const uint16_t max_payload = HOSTLINK_TX_DMA_CHUNK - 10; // 留出帧头和CRC等空间
    uint8_t packet_index = 0;
    uint16_t sent_bytes = 0;
    uint8_t buf[HOSTLINK_TX_DMA_CHUNK];

    while (sent_bytes < reg_count * (/* 每个变量最小字节数 */ 6)) {
        // 构造帧头
        buf[0] = SYNC1; buf[1] = SYNC2; buf[2] = CMD_LIST_VARS;
        // 预留长度 (3,4) 和分片信息 (5,6)
        uint16_t idx = 7; // 从7开始放变量数据
        int count_in_packet = 0;
        int start_index = sent_bytes; // 简化：直接按变量序号遍历
        for (int i = start_index; i < reg_count && idx < max_payload; i++) {
            // 计算该变量占用字节数：id(2)+type(1)+perm(1)+name_len(1)+name
            uint8_t name_len = strlen(registry[i].name);
            if (idx + 5 + name_len > max_payload) break;
            buf[idx++] = registry[i].id & 0xFF;
            buf[idx++] = (registry[i].id >> 8) & 0xFF;
            buf[idx++] = registry[i].type;
            buf[idx++] = registry[i].permission;
            buf[idx++] = name_len;
            memcpy(&buf[idx], registry[i].name, name_len);
            idx += name_len;
            sent_bytes += (5 + name_len); // 实际不精确，用变量计数更简单
            count_in_packet++;
        }
        // 计算总包数：粗略估算，假设每个变量最小6字节
        int total = (reg_count * 6) / max_payload + 1;
        buf[5] = total;
        buf[6] = packet_index++;
        uint16_t payload_len = idx - 7;
        buf[3] = payload_len & 0xFF;
        buf[4] = (payload_len >> 8) & 0xFF;
        // 发送（异步，不阻塞）
        DataLink_SendPacket(buf, idx);
        // 如果没有继续发送（这里简单退化为只发一包，后续可扩展）
        break;
    }
    xSemaphoreGive(var_mutex);
}

void VAR_GetSubscribedList(uint16_t *ids, uint8_t *count) {

    xSemaphoreTake(var_mutex, portMAX_DELAY);

    memcpy(ids, subscribed, sub_count * sizeof(uint16_t));
    *count = sub_count;

    xSemaphoreGive(var_mutex);
}