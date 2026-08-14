#if defined(__CC_ARM) || defined(__ARMCC_VERSION)
#pragma diag_suppress 1
#endif
#ifndef SECURITY_H
#define SECURITY_H

#include <stdint.h>
#include <stdbool.h>
#include "ota_source.h"

/* 固件头部定义 */
#pragma pack(1)
typedef struct {
    uint32_t magic;           // 0x4F5441FE
    uint32_t version;         // 功能版本号（防回滚）
    uint32_t firmware_size;   // 原始固件大小
    uint8_t  aes_iv[12];      // AES-CTR 初始化向量
    uint32_t chip_id;         // 目标芯片 IDCODE 低 12 位（防跨芯片烧录）
    uint32_t build_no;        // 单调递增构建号（防重放）
} ota_header_t;
#pragma pack()

#define OTA_HEADER_SIZE   sizeof(ota_header_t)
#define OTA_SIGN_SIZE     64
/* 安全验证结果码（供上位机状态帧精确诊断） */
#define SEC_ERR_MAGIC      -1
#define SEC_ERR_CHIP       -2
#define SEC_ERR_REPLAY     -3
#define SEC_ERR_SHA        -4
#define SEC_ERR_ECDSA      -5
#define SEC_ERR_ROLLBACK   -6
#define SEC_ERR_SIZE       -7   /* decrypted firmware exceeds RUN area */

/* 解密函数，由 main.c 调用 */
bool aes_ctr_decrypt_to_flash(const ota_source_t *src, uint32_t data_off,
                              uint32_t len, const uint8_t *key,
                              const uint8_t *iv16, uint32_t dest_addr);

/* 固定 AES 密钥（测试用，后续改为 UID 派生） */
extern const uint8_t AES_KEY[32];
/* 安全处理入口：返回 0=成功，负值=SEC_ERR_*（具体失败原因） */
int32_t security_verify_and_decrypt(const ota_source_t *src, uint32_t *out_size,
                                    uint32_t current_version,
                                    uint32_t last_build_no);
/**
 * @brief  使用芯片 UID 派生 256 位 AES 密钥
 * @note   与上位机使用相同的盐值
 */
void derive_aes_key(uint8_t key[32]);

#endif
