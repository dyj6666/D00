#ifndef __CRC32_H
#define __CRC32_H
#include <stdint.h>

void     crc32_init(uint32_t *crc);
void     crc32_update(uint32_t *crc, const uint8_t *data, uint32_t len);
uint32_t crc32_finalize(uint32_t *crc);

#endif