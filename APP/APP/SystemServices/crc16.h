/* ================================================================
 * crc16 —— CRC-16/MODBUS 纯逻辑实现
 *
 * 架构位置：APP 服务层；HOSTLINK 帧校验，主机单测覆盖
 * ================================================================ */
#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>

uint16_t CRC16_Calculate(const uint8_t *data, uint16_t len);

#endif
