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
/* 解密函数，由 main.c 调用 */
bool aes_ctr_decrypt_to_flash(uint32_t src_addr, uint32_t len,
                              const uint8_t *key, const uint8_t *iv16,
                              uint32_t dest_addr);

/* 固定 AES 密钥（测试用，后续改为 UID 派生） */
extern const uint8_t AES_KEY[32];
/* 安全处理入口 */
bool security_verify_and_decrypt(uint32_t download_addr, uint32_t *out_size, uint32_t current_version);
/**
 * @brief  使用芯片 UID 派生 256 位 AES 密钥
 * @note   与上位机使用相同的盐值
 */
void derive_aes_key(uint8_t key[32]);

#endif