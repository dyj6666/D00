#include "var_manager.h"
#include "app_config.h"
#include "data_link.h"
#include "FreeRTOS.h"
#include "logger.h"
#include "semphr.h"
#include "var_list.h"

#include <string.h>

/* 变量名最大长度（保证单条目编码 ≤ 5+32=37 字节，任何包都装得下） */
#define VAR_NAME_MAX_LEN 32

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
    if (name == NULL || ptr == NULL || strlen(name) == 0 || strlen(name) > VAR_NAME_MAX_LEN) {
        return -3;
    }

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
                    *(uint8_t *)buf = *(uint8_t *)registry[i].ptr;
                    *len = 1;
                    ret = 0;
                    break;
                case VAR_TYPE_INT16:
                    *(int16_t *)buf = *(int16_t *)registry[i].ptr;
                    *len = 2;
                    ret = 0;
                    break;
                case VAR_TYPE_INT32:
                    *(int32_t *)buf = *(int32_t *)registry[i].ptr;
                    *len = 4;
                    ret = 0;
                    break;
                case VAR_TYPE_FLOAT:
                    *(float *)buf = *(float *)registry[i].ptr;
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

/* ---------- 发送变量列表（完整分片） ---------- */
void VAR_SendList(void)
{
    if (xSemaphoreTake(var_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        LOG_Printf("VAR_SendList timeout\r\n");
        return;
    }

    /* 整帧上限 = DMA 块 - CRC 两字节 */
    const uint16_t max_frame_len = HOSTLINK_TX_DMA_CHUNK - 2;
    uint8_t total = VarList_TotalPackets(registry, reg_count, max_frame_len);
    uint8_t buf[HOSTLINK_TX_DMA_CHUNK];

    for (uint8_t pkt = 0; pkt < total; pkt++) {
        uint16_t len = 0;
        if (VarList_BuildPacket(registry, reg_count, max_frame_len,
                                total, pkt, buf, sizeof(buf), &len) == 0) {
            DataLink_SendPacket(buf, len);
        }
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
