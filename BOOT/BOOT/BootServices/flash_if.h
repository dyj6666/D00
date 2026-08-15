/**
 * @file    flash_if.h
 * @brief   Internal Flash interface (Register-level, Industrial Grade)
 */

#ifndef FLASH_IF_H
#define FLASH_IF_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx_hal.h"

bool flash_erase(uint32_t start_addr, uint32_t end_addr);
bool flash_write(uint32_t addr, const uint8_t *data, uint32_t len);
bool flash_copy_raw(uint32_t dest, uint32_t src, uint32_t len);

/* 扇区号映射（F407 1MB 布局）：全 BOOT 唯一实现，
 * 其他模块（如 boot_param 擦除参数扇区）必须复用，禁止私抄映射链。 */
uint32_t flash_get_sector(uint32_t addr);

#endif



