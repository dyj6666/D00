#include "flash_if.h"

/*----------------------------------------------------------------------------
 * Sector mapping (STM32F40x 1MB flash)
 *----------------------------------------------------------------------------*/
static uint32_t flash_get_sector(uint32_t addr) {
    if (addr < 0x08004000) return FLASH_SECTOR_0;
    if (addr < 0x08008000) return FLASH_SECTOR_1;
    if (addr < 0x0800C000) return FLASH_SECTOR_2;
    if (addr < 0x08010000) return FLASH_SECTOR_3;
    if (addr < 0x08020000) return FLASH_SECTOR_4;
    if (addr < 0x08040000) return FLASH_SECTOR_5;
    if (addr < 0x08060000) return FLASH_SECTOR_6;
    if (addr < 0x08080000) return FLASH_SECTOR_7;
    if (addr < 0x080A0000) return FLASH_SECTOR_8;
    if (addr < 0x080C0000) return FLASH_SECTOR_9;
    if (addr < 0x080E0000) return FLASH_SECTOR_10;
    return FLASH_SECTOR_11;
}

/*----------------------------------------------------------------------------
 * Erase sectors (uses HAL, but safe)
 *----------------------------------------------------------------------------*/
bool flash_erase(uint32_t start_addr, uint32_t end_addr) {
    FLASH_EraseInitTypeDef erase_init;
    uint32_t sector_error = 0;

    HAL_FLASH_Unlock();

    erase_init.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase_init.Sector       = flash_get_sector(start_addr);
    erase_init.NbSectors    = flash_get_sector(end_addr) - flash_get_sector(start_addr) + 1;
    erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&erase_init, &sector_error) != HAL_OK) {
        HAL_FLASH_Lock();
        return false;
    }

    HAL_FLASH_Lock();
    return true;
}

/*----------------------------------------------------------------------------
 * Program one word (register-level, no HAL)
 *----------------------------------------------------------------------------*/
static bool flash_program_word(uint32_t addr, uint32_t data) {
    /* Wait until flash is not busy */
    while (FLASH->SR & FLASH_SR_BSY);

    /* Clear any previous error flags */
    FLASH->SR = (FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR);

    /* Enable programming */
    FLASH->CR |= FLASH_CR_PG;

    /* Write the word */
    *(volatile uint32_t *)addr = data;

    /* Wait for completion */
    while (FLASH->SR & FLASH_SR_BSY);

    /* Check for errors */
    uint32_t sr = FLASH->SR;
    if (sr & (FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR)) {
        FLASH->CR &= ~FLASH_CR_PG;
        return false;
    }

    /* Disable programming */
    FLASH->CR &= ~FLASH_CR_PG;
    return true;
}

/*----------------------------------------------------------------------------
 * Write bytes (uses register-level program, interrupts disabled)
 *----------------------------------------------------------------------------*/
bool flash_write(uint32_t addr, const uint8_t *data, uint32_t len) {
    if (len == 0) return true;

    __disable_irq();

    /* Unlock flash */
    FLASH->KEYR = 0x45670123;
    FLASH->KEYR = 0xCDEF89AB;

    /* Handle leading unaligned bytes */
    while ((addr & 0x03) && (len > 0)) {
        uint32_t word_addr = addr & ~0x03;
        uint32_t offset    = addr & 0x03;
        uint32_t existing  = *(volatile uint32_t *)word_addr;
        uint8_t *bytes     = (uint8_t *)&existing;
        bytes[offset]      = *data;

        if (!flash_program_word(word_addr, existing)) {
            FLASH->CR |= FLASH_CR_LOCK;
            __enable_irq();
            return false;
        }
        addr++; data++; len--;
    }

    /* Write whole words */
    while (len >= 4) {
        uint32_t word = *(uint32_t *)data;
        if (!flash_program_word(addr, word)) {
            FLASH->CR |= FLASH_CR_LOCK;
            __enable_irq();
            return false;
        }
        addr += 4; data += 4; len -= 4;
        IWDG->KR = 0xAAAA;   /* feed watchdog directly */
    }

    /* Trailing bytes */
    while (len > 0) {
        uint32_t word_addr = addr & ~0x03;
        uint32_t offset    = addr & 0x03;
        uint32_t existing  = *(volatile uint32_t *)word_addr;
        uint8_t *bytes     = (uint8_t *)&existing;
        bytes[offset]      = *data;

        if (!flash_program_word(word_addr, existing)) {
            FLASH->CR |= FLASH_CR_LOCK;
            __enable_irq();
            return false;
        }
        addr++; data++; len--;
        IWDG->KR = 0xAAAA;
    }

    /* Lock flash and restore interrupts */
    FLASH->CR |= FLASH_CR_LOCK;
    __enable_irq();
    return true;
}

/*----------------------------------------------------------------------------
 * Copy raw flash (register-level, no HAL, interrupts disabled)
 *----------------------------------------------------------------------------*/
bool flash_copy_raw(uint32_t dest, uint32_t src, uint32_t len) {
    if (len == 0) return true;

    __disable_irq();

    /* Unlock flash */
    FLASH->KEYR = 0x45670123;
    FLASH->KEYR = 0xCDEF89AB;

    uint32_t i = 0;
    while (i < len) {
        uint32_t word;

        /* Handle last word (1..4 bytes) */
        if (i + 4 > len) {
            uint32_t remaining = len - i;
            word = *(volatile uint32_t *)(dest + i);   /* read-modify-write */
            uint8_t *dst_bytes = (uint8_t *)&word;
            for (uint32_t j = 0; j < remaining; j++) {
                dst_bytes[j] = *(volatile uint8_t *)(src + i + j);
            }
        } else {
            word = *(volatile uint32_t *)(src + i);
        }

        /* Program the word */
        while (FLASH->SR & FLASH_SR_BSY);
        FLASH->SR = (FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR);
        FLASH->CR |= FLASH_CR_PG;
        *(volatile uint32_t *)(dest + i) = word;
        while (FLASH->SR & FLASH_SR_BSY);

        if (FLASH->SR & (FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR)) {
            FLASH->CR &= ~FLASH_CR_PG;
            FLASH->CR |= FLASH_CR_LOCK;
            __enable_irq();
            return false;
        }

        FLASH->CR &= ~FLASH_CR_PG;
        i += 4;
        IWDG->KR = 0xAAAA;
    }

    FLASH->CR |= FLASH_CR_LOCK;
    __enable_irq();
    return true;
}

