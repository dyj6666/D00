#ifndef MY_SHA256_H
#define MY_SHA256_H

#include <stdint.h>

void sha256(const uint8_t *data, uint32_t len, uint8_t digest[32]);

#endif

