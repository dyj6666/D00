#ifndef MY_SHA256_H
#define MY_SHA256_H

#include <stdint.h>
#include <stddef.h>

void sha256(const uint8_t *data, uint32_t len, uint8_t digest[32]);

/* 流式 SHA256（外部 Flash 大包哈希用）：init/update/final */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buf[64];
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32]);

#endif

