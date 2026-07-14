#include "var_manager.h"
#include "app_config.h"
#include <string.h>

static VarEntry registry[HOSTLINK_MAX_VARS];
static uint8_t reg_count = 0;

static uint16_t subscribed[HOSTLINK_MAX_SUBSCRIBE];
static uint8_t sub_count = 0;

void VAR_Init(void) {
    reg_count = 0;
    sub_count = 0;
}

int VAR_Register(uint16_t id, const char *name, VarType type, uint8_t perm, void *ptr) {
    if (reg_count >= HOSTLINK_MAX_VARS) return -1;
    registry[reg_count].id = id;
    registry[reg_count].name = name;
    registry[reg_count].type = type;
    registry[reg_count].permission = perm;
    registry[reg_count].ptr = ptr;
    reg_count++;
    return 0;
}

int VAR_Read(uint16_t id, void *buf, uint16_t *len) {
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
    return -1;
}

int VAR_Write(uint16_t id, const void *buf, uint16_t len) {
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
    return -1;
}

void VAR_Subscribe(uint16_t id) {
    if (sub_count >= HOSTLINK_MAX_SUBSCRIBE) return;
    subscribed[sub_count++] = id;
}

void VAR_ClearSubscriptions(void) {
    sub_count = 0;
}

void VAR_GetSubscribedList(uint16_t *ids, uint8_t *count) {
    memcpy(ids, subscribed, sub_count * sizeof(uint16_t));
    *count = sub_count;
}