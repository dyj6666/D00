/**
 * @file    security.c
 * @brief   OTA 安全验证与解密模块 (工业级/去调试版)
 * @note    已移除所有详细调试打印，仅保留关键状态日志。
 *          使用 TinyAES (AES256) + 软件 SHA256。
 *          ECDSA 验签暂未启用，待后续集成。
 */

#define AES256 1
#include <stdio.h>
#include <string.h>
#include "security.h"
#include "flash_if.h"
#include "stm32f4xx_hal.h"
#include "aes.h"
#include "my_sha256.h"          // 你之前的软件 SHA256
#include "uECC.h"
#include "boot_config.h"
/* ECDSA 公钥 (64字节，由 gen_keys.py 生成，需替换为实际值) */
static const uint8_t ECDSA_PUB_KEY[64] = {
    0xFA, 0x80, 0xFA, 0x3F, 0xCD, 0xB0, 0xBD, 0x52,
    0x69, 0xB1, 0xC9, 0x0F, 0xFB, 0xD7, 0x11, 0x31,
    0x74, 0x11, 0xE4, 0xD8, 0xE4, 0x14, 0x72, 0xD0,
    0x0C, 0x71, 0xD5, 0x71, 0xF5, 0x05, 0xD0, 0xF0,
    0xC9, 0xA4, 0x10, 0xF5, 0x06, 0xBC, 0xDC, 0xD3,
    0x93, 0x00, 0x48, 0xF3, 0xC9, 0x76, 0x24, 0x4B,
    0x9F, 0x68, 0x39, 0xA1, 0x7B, 0x71, 0xC1, 0x83,
    0x11, 0x0A, 0x6C, 0x68, 0xAC, 0xA0, 0x47, 0xAF
};

/**
 * @brief  使用芯片 UID 派生 256 位 AES 密钥
 * @note   与上位机使用相同的盐值
 */
void derive_aes_key(uint8_t key[32]) {
    uint32_t uid[3];
    uid[0] = *(volatile uint32_t *)0x1FFF7A10;
    uid[1] = *(volatile uint32_t *)0x1FFF7A14;
    uid[2] = *(volatile uint32_t *)0x1FFF7A18;
    // 固定盐值，与 Python 完全一致（15 字节，不含结尾额外空字符）
    const char *salt = "OTA-AES-KEY-V1";   // 14 字符 + 自动 '\0' = 15 字节
    uint8_t buffer[12 + 15];               // 总共 27 字节
    memcpy(buffer, uid, sizeof(uid));      // 12 字节
    memcpy(buffer + 12, salt, 15);         // 15 字节（盐）
    sha256(buffer, 27, key);               // 计算 SHA256(27 字节)
    
    printf("AES_KEY_PREFIX: ");
    for (int i = 0; i < 8; i++) printf("%02X", key[i]);
    printf("\r\n");
}

/* 声明 uECC_secp256r1 曲线函数 */
const struct uECC_Curve_t *uECC_secp256r1(void);

/**
 * @brief  软件 SHA256（从 Flash 地址直接计算）
 */
static bool sw_sha256(uint32_t start_addr, uint32_t len, uint8_t digest[32]) {
    sha256((const uint8_t *)start_addr, len, digest);
    return true;
}

/**
 * @brief  AES-CTR 解密并写入 Flash
 * @param  src_addr  密文源地址（Download 区）
 * @param  len       长度
 * @param  key       32 字节 AES 密钥
 * @param  iv16      16 字节 IV（已补零）
 * @param  dest_addr 目标地址（APP 区）
 */
bool aes_ctr_decrypt_to_flash(uint32_t src_addr, uint32_t len,
                                     const uint8_t *key, const uint8_t *iv16,
                                     uint32_t dest_addr) {
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, key, iv16);

    uint8_t buf[256];
    uint32_t offset = 0;

    while (offset < len) {
        uint32_t chunk = len - offset;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);

        // 读取密文
        for (uint32_t i = 0; i < chunk; i++) {
            buf[i] = *((volatile uint8_t *)(src_addr + offset + i));
        }

        // 解密
        AES_CTR_xcrypt_buffer(&ctx, buf, chunk);

        // 写入 APP 区
        if (!flash_write(dest_addr + offset, buf, chunk)) {
            printf("[SEC] Flash write failed at offset %lu!\r\n", offset);
            return false;
        }
        offset += chunk;
        IWDG->KR = 0xAAAA;   /* 喂狗 */
    }

    return true;
}

/**
 * @brief  ECDSA 验签
 */
static bool ecdsa_verify(const uint8_t *pubkey, const uint8_t *hash, const uint8_t *sig) {
    return (uECC_verify(pubkey, hash, 32, sig, uECC_secp256r1()) != 0);
}

/**
 * @brief  安全验证（验签 + 版本检查），不执行 Flash 写入
 * @retval true  验证通过，*out_size 为原始固件大小
 *         false 验证失败
 */
bool security_verify_and_decrypt(uint32_t download_addr, uint32_t *out_size, uint32_t current_version) {
    ota_header_t header;
    memcpy(&header, (void *)download_addr, sizeof(header));
    if (header.magic != 0x4F5441FE) {
        printf("[SEC] Magic mismatch!\r\n");
        return false;
    }

    uint32_t body_len = header.firmware_size;
    uint32_t total_len = sizeof(header) + body_len;
    uint8_t hash[32];
    if (!sw_sha256(download_addr, total_len, hash)) {
        printf("[SEC] SHA256 failed!\r\n");
        return false;
    }

    uint8_t *sig = (uint8_t *)(download_addr + total_len);
    if (!ecdsa_verify(ECDSA_PUB_KEY, hash, sig)) {
        printf("[SEC] ECDSA verify failed!\r\n");
        return false;
    }

    // 版本防回滚
    if (header.version < current_version) {
        printf("[SEC] Rollback denied! New:%lu, Current:%lu\r\n", header.version, current_version);
        return false;
    }

    *out_size = body_len;
    return true;
}