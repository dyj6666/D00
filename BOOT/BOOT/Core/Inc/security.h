#pragma diag_suppress 1
#ifndef SECURITY_H
#define SECURITY_H

#include <stdint.h>
#include <stdbool.h>

/* 固件头部定义 */
#pragma pack(1)
typedef struct {
    uint32_t magic;           // 0x4F5441FE
    uint32_t version;
    uint32_t firmware_size;   // 原始固件大小
    uint8_t  aes_iv[12];      // AES-CTR 初始化向量
    uint8_t  reserved[8];     // 保留
} ota_header_t;
#pragma pack()

#define OTA_HEADER_SIZE   sizeof(ota_header_t)
#define OTA_SIGN_SIZE     64

/* 安全处理入口 */
bool security_verify_and_decrypt(uint32_t download_addr, uint32_t app_addr, uint32_t *out_size);

#endif