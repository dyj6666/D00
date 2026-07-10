#define AES256 1
#include <stdio.h>
#include <string.h>
#include "security.h"
#include "flash_if.h"
#include "stm32f4xx_hal.h"
#include "aes.h"
#include "sha256.h"

/* 固定 AES 密钥 (测试用，需与上位机一致) */
static const uint8_t AES_KEY[32] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
};

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
static bool aes_ctr_decrypt_to_flash(uint32_t src_addr, uint32_t len,
                                     const uint8_t *key, const uint8_t *iv16,
                                     uint32_t dest_addr) {
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, key, iv16);

    uint8_t buf[256];
    uint32_t offset = 0;
    bool first_block = true;

    while (offset < len) {
        uint32_t chunk = len - offset;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);

        // 读取密文
        for (uint32_t i = 0; i < chunk; i++) {
            buf[i] = *((volatile uint8_t *)(src_addr + offset + i));
        }

        // 解密
        AES_CTR_xcrypt_buffer(&ctx, buf, chunk);

        if (first_block) {
            printf("[AES] First 32 decrypted bytes: ");
            for (int i = 0; i < (chunk < 32 ? chunk : 32); i++) printf("%02X ", buf[i]);
            printf("\r\n");
            first_block = false;
        }

        // 写入 APP 区
        if (!flash_write(dest_addr + offset, buf, chunk)) {
            printf("[SEC] Flash write failed at offset %lu!\r\n", offset);
            return false;
        }
        offset += chunk;
        IWDG->KR = 0xAAAA;
    }

    // 回读验证
    printf("[SEC] APP first 32 bytes (verify): ");
    for (int i = 0; i < 32; i++) {
        printf("%02X ", *((volatile uint8_t *)(dest_addr + i)));
    }
    printf("\r\n");

    return true;
}

/**
 * @brief  安全验证与解密总入口（验签暂跳过）
 */
bool security_verify_and_decrypt(uint32_t download_addr, uint32_t app_addr, uint32_t *out_size) {
    ota_header_t header;

    memcpy(&header, (void *)download_addr, sizeof(header));
    printf("[SEC] Magic: 0x%08X\r\n", (unsigned)header.magic);
    if (header.magic != 0x4F5441FE) {
        printf("[SEC] Magic mismatch!\r\n");
        return false;
    }

    printf("[SEC] Version: %lu, FirmwareSize: %lu\r\n", header.version, header.firmware_size);
    printf("[SEC] IV: ");
    for (int i = 0; i < 12; i++) printf("%02X ", header.aes_iv[i]);
    printf("\r\n");

    // 打印加密体前 32 字节
    printf("[SEC] Encrypted body first 32 bytes: ");
    for (int i = 0; i < 32; i++) {
        printf("%02X ", *((volatile uint8_t *)(download_addr + sizeof(header) + i)));
    }
    printf("\r\n");

    // 构造 16 字节 IV（TinyAES 需要 16 字节，后 4 字节补零）
    uint8_t iv16[16];
    memcpy(iv16, header.aes_iv, 12);
    memset(iv16 + 12, 0, 4);

    // 解密并写入 APP 区
    if (!aes_ctr_decrypt_to_flash(download_addr + sizeof(header),
                                  header.firmware_size,
                                  AES_KEY,
                                  iv16,
                                  app_addr)) {
        printf("[SEC] AES decrypt failed!\r\n");
        return false;
    }

    *out_size = header.firmware_size;
    printf("[SEC] Decryption and write complete.\r\n");
    return true;
}