/* ================================================================
 * crc16 —— CRC-16/MODBUS 纯逻辑实现（查表/位算法）
 *
 * 架构位置：APP 服务层；HOSTLINK 帧校验
 * ================================================================ */
#include "crc16.h"
#include "app_config.h"

uint16_t CRC16_Calculate(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= (uint16_t)*data++;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ HOSTLINK_CRC_POLY;   /* 右移 + 反转多项式 */
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}
