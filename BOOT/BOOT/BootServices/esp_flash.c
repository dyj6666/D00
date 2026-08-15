/* ================================================================
 * esp_flash —— BOOT 侧板载外部 Flash 访问层实现
 *
 * 传输：SPI1 主模式 8bit 42MHz（APB2/2），CPOL0/CPHA0，寄存器级
 *       阻塞轮询（TXE/RXNE），带超时；BOOT 无 OS，长操作喂 IWDG。
 * 命令：0x03 读 / 0x02 页写 / 0x20 扇区擦 / 0x05 读状态 / 0x06 写使能。
 * ================================================================ */
#include "esp_flash.h"
#include "security.h"
#include "stm32f4xx_hal.h"
#include "iwdg.h"

#include <string.h>

/* ---------------- W25Q128 命令 ---------------- */
#define ESP_CMD_READ          0x03u
#define ESP_CMD_PAGE_PROGRAM  0x02u
#define ESP_CMD_SECTOR_ERASE  0x20u
#define ESP_CMD_BLOCK_ERASE   0xD8u
#define ESP_CMD_WRITE_ENABLE  0x06u
#define ESP_CMD_READ_STATUS   0x05u
#define ESP_CMD_JEDEC_ID      0x9Fu

#define ESP_SECTOR            4096u
#define ESP_PAGE              256u
#define ESP_SR_WIP            (1u << 0)
#define ESP_SR_WEL            (1u << 1)

static bool s_ready;

/* ---------------- 片选 ---------------- */
static void cs_low(void)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
}

static void cs_high(void)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
}

/* ---------------- SPI 传输（逐字节，带超时） ---------------- */
static bool spi_wait(uint32_t mask, uint32_t guard)
{
    while ((SPI1->SR & mask) == 0u) {
        if (--guard == 0u) {
            return false;
        }
    }
    return true;
}

static bool spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    SPI1->CR1 |= SPI_CR1_SPE;   /* HAL 初始化不置 SPE */
    for (uint32_t i = 0; i < len; i++) {
        if (!spi_wait(SPI_SR_TXE, 100000u)) {
            return false;
        }
        SPI1->DR = (tx != NULL) ? tx[i] : 0xFFu;
        if (!spi_wait(SPI_SR_RXNE, 100000u)) {
            return false;
        }
        if (rx != NULL) {
            rx[i] = (uint8_t)SPI1->DR;
        } else {
            (void)SPI1->DR;
        }
    }
    return true;
}

/* ---------------- 状态/等待忙 ---------------- */
/* 毫秒级超时：W25Q128 64KB 块擦标称 max 2s，取 5s 裕量；
 * 不再用迭代计数（次数与时钟频率耦合，最坏情况可能误判失败）。 */
static bool esp_wait_busy(uint32_t timeout_ms)
{
    uint8_t cmd = ESP_CMD_READ_STATUS;
    uint8_t sr = 0u;
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < timeout_ms) {
        cs_low();
        if (!spi_xfer(&cmd, NULL, 1u) || !spi_xfer(NULL, &sr, 1u)) {
            cs_high();
            return false;
        }
        cs_high();
        if ((sr & ESP_SR_WIP) == 0u) {
            return true;
        }
        IWDG->KR = 0xAAAA;   /* 长等待喂狗 */
    }
    return false;
}

/* ---------------- 生命周期 ---------------- */
bool EspFlash_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* SPI1 复用：PB3=SCK / PB4=MISO / PB5=MOSI（AF5） */
    gpio.Pin = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* CS：PB14 推挽输出，初始拉高 */
    gpio.Pin = GPIO_PIN_14;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_MEDIUM;
    HAL_GPIO_Init(GPIOB, &gpio);
    cs_high();

    /* SPI1 主模式 8bit 42MHz（BR=2） */
    SPI1->CR1 = 0u;
    SPI1->CR2 = 0u;
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSI | SPI_CR1_SSM |
                SPI_CR1_SPE;   /* BR=000 -> /2 = 42MHz */

    /* 探测 JEDEC ID */
    uint8_t id[3] = {0u, 0u, 0u};
    uint8_t cmd = ESP_CMD_JEDEC_ID;
    cs_low();
    bool ok = spi_xfer(&cmd, NULL, 1u) && spi_xfer(NULL, id, 3u);
    cs_high();
    if (!ok || id[0] != 0xEFu || id[1] != 0x40u || id[2] != 0x18u) {
        return false;
    }
    s_ready = true;
    return true;
}

/* ---------------- 读 ---------------- */
bool EspFlash_Read(uint32_t off, void *buf, uint32_t len)
{
    if (!s_ready || buf == NULL || len == 0u) {
        return false;
    }
    uint8_t hdr[4] = {
        ESP_CMD_READ,
        (uint8_t)(off >> 16), (uint8_t)(off >> 8), (uint8_t)off,
    };
    cs_low();
    bool ok = spi_xfer(hdr, NULL, sizeof(hdr)) &&
              spi_xfer(NULL, (uint8_t *)buf, len);
    cs_high();
    return ok;
}

/* ---------------- 写使能 ---------------- */
static bool esp_write_enable(void)
{
    uint8_t cmd = ESP_CMD_WRITE_ENABLE;
    cs_low();
    bool ok = spi_xfer(&cmd, NULL, 1u);
    cs_high();
    if (!ok) {
        return false;
    }
    uint8_t sr = 0u;
    uint8_t sc = ESP_CMD_READ_STATUS;
    cs_low();
    ok = spi_xfer(&sc, NULL, 1u) && spi_xfer(NULL, &sr, 1u);
    cs_high();
    return ok && (sr & ESP_SR_WEL) != 0u;
}

/* ---------------- 写（页编程，自动跨页） ---------------- */
bool EspFlash_Write(uint32_t off, const void *data, uint32_t len)
{
    if (!s_ready || data == NULL || len == 0u) {
        return false;
    }
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0u) {
        if (!esp_write_enable()) {
            return false;
        }
        uint32_t page_off = off & (ESP_PAGE - 1u);
        uint32_t chunk = ESP_PAGE - page_off;
        if (chunk > len) {
            chunk = len;
        }
        uint8_t hdr[4] = {
            ESP_CMD_PAGE_PROGRAM,
            (uint8_t)(off >> 16), (uint8_t)(off >> 8), (uint8_t)off,
        };
        cs_low();
        bool ok = spi_xfer(hdr, NULL, sizeof(hdr)) &&
                  spi_xfer(p, NULL, chunk);
        cs_high();
        if (!ok || !esp_wait_busy(5000u)) {
            return false;
        }
        off += chunk;
        p += chunk;
        len -= chunk;
    }
    return true;
}

/* ---------------- 4KB 扇区擦除 ---------------- */
bool EspFlash_EraseSector(uint32_t off)
{
    if (!s_ready || (off & (ESP_SECTOR - 1u)) != 0u) {
        return false;
    }
    if (!esp_write_enable()) {
        return false;
    }
    uint8_t hdr[4] = {
        ESP_CMD_SECTOR_ERASE,
        (uint8_t)(off >> 16), (uint8_t)(off >> 8), (uint8_t)off,
    };
    cs_low();
    bool ok = spi_xfer(hdr, NULL, sizeof(hdr));
    cs_high();
    return ok && esp_wait_busy(5000u);
}

/* ---------------- 64KB 块擦除（大区域备份/清槽提速） ---------------- */
bool EspFlash_EraseBlock64(uint32_t off)
{
    if (!s_ready || (off & (ESP_BLOCK64_SIZE - 1u)) != 0u) {
        return false;
    }
    if (!esp_write_enable()) {
        return false;
    }
    uint8_t hdr[4] = {
        ESP_CMD_BLOCK_ERASE,
        (uint8_t)(off >> 16), (uint8_t)(off >> 8), (uint8_t)off,
    };
    cs_low();
    bool ok = spi_xfer(hdr, NULL, sizeof(hdr));
    cs_high();
    return ok && esp_wait_busy(5000u);
}

/* ---------------- 64KB 块粒度区域擦除（逐块喂狗） ---------------- */
bool EspFlash_EraseRange64(uint32_t off, uint32_t len)
{
    if (!s_ready || len == 0u ||
        (off & (ESP_BLOCK64_SIZE - 1u)) != 0u ||
        (len & (ESP_BLOCK64_SIZE - 1u)) != 0u) {
        return false;
    }
    for (uint32_t done = 0u; done < len; done += ESP_BLOCK64_SIZE) {
        if (!EspFlash_EraseBlock64(off + done)) {
            return false;
        }
    }
    return true;
}

/* ---------------- 探测外部槽 0 有效包 ---------------- */
bool EspFlash_HasPackage(uint32_t *out_total_size)
{
    if (!s_ready) {
        return false;
    }
    uint8_t hdr[16];
    if (!EspFlash_Read(ESP_OTA_BASE, hdr, sizeof(hdr))) {
        return false;
    }
    /* ota_header_t：magic @0，firmware_size @8（与 security.h 一致） */
    uint32_t magic = 0u;
    memcpy(&magic, hdr, 4);
    if (magic != 0x4F5441FEu) {
        return false;
    }
    uint32_t fw_size = 0u;
    memcpy(&fw_size, hdr + 8, 4);   /* ota_header_t.firmware_size 偏移 8 */
    if (fw_size == 0u ||
        fw_size + OTA_HEADER_SIZE + OTA_SIGN_SIZE > ESP_OTA_DL_SIZE) {
        return false;
    }
    if (out_total_size != NULL) {
        *out_total_size = OTA_HEADER_SIZE + fw_size + OTA_SIGN_SIZE;
    }
    return true;
}
