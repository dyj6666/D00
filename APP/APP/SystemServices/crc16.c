#include "crc16.h"
#include "app_config.h"   // 包含配置

uint16_t CRC16_Calculate(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= (uint16_t)*data++;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x0001)          // 检查最低位
                crc = (crc >> 1) ^ HOSTLINK_CRC_POLY;  // 右移，异或反转多项式
            else
                crc >>= 1;
        }
    }
    return crc;
}