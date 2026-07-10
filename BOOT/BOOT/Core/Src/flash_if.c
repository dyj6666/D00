#include "flash_if.h"
#include "stm32f4xx_hal.h"   // 已经包含

/* 放在 SRAM 中执行的喂狗函数，确保 Flash 擦除期间可用 */
__attribute__((long_call, section(".ramfunc")))
static void ram_feed_dog(void) {
    IWDG->KR = 0xAAAA;
}
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

/**
 * @brief  Erase multiple sectors using HAL, feeding watchdog each sector
 * @param  start_addr  Start address (must be sector-aligned)
 * @param  end_addr    End address (inclusive)
 * @retval true  success
 * @retval false error
 */
bool flash_erase(uint32_t start_addr, uint32_t end_addr) {
    uint32_t start_sector = flash_get_sector(start_addr);
    uint32_t end_sector   = flash_get_sector(end_addr);

    for (uint32_t sec = start_sector; sec <= end_sector; sec++) {
        /* 擦除前喂狗 */
        IWDG->KR = 0xAAAA;

        /* 解锁 */
        FLASH->KEYR = 0x45670123;
        FLASH->KEYR = 0xCDEF89AB;

        /* 清除错误标志 */
        FLASH->SR = (FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR);

        /* 设置扇区并启动擦除 */
        FLASH->CR &= ~FLASH_CR_SNB;
        FLASH->CR |= (sec << FLASH_CR_SNB_Pos);
        FLASH->CR |= FLASH_CR_SER;
        FLASH->CR |= FLASH_CR_STRT;

        /* 等待完成，并在 RAM 中喂狗 */
        while (FLASH->SR & FLASH_SR_BSY) {
            ram_feed_dog();   /* 此函数必须在 RAM 中 */
        }

        /* 关闭 SER 使能 */
        FLASH->CR &= ~FLASH_CR_SER;

        if (FLASH->SR & (FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR)) {
            FLASH->CR |= FLASH_CR_LOCK;
            return false;
        }

        FLASH->CR |= FLASH_CR_LOCK;

        /* 擦除后再喂狗 */
        IWDG->KR = 0xAAAA;
    }
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
    HAL_FLASH_Unlock();

    /* 处理前导字节，对齐到字边界 */
    while ((addr & 0x03) && (len > 0)) {
        uint32_t word_addr = addr & ~0x03;
        uint32_t offset    = addr & 0x03;
        uint32_t existing  = *(volatile uint32_t *)word_addr;
        uint8_t *bytes     = (uint8_t *)&existing;
        bytes[offset]      = *data;

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, word_addr, existing) != HAL_OK) {
            HAL_FLASH_Lock();
            __enable_irq();
            return false;
        }
        addr++; data++; len--;
        IWDG->KR = 0xAAAA;
    }

    /* 写入整字 */
    while (len >= 4) {
        uint32_t word = *(uint32_t *)data;
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, word) != HAL_OK) {
            HAL_FLASH_Lock();
            __enable_irq();
            return false;
        }
        addr += 4; data += 4; len -= 4;
        IWDG->KR = 0xAAAA;
    }

    /* 处理尾部字节 */
    while (len > 0) {
        uint32_t word_addr = addr & ~0x03;
        uint32_t offset    = addr & 0x03;
        uint32_t existing  = *(volatile uint32_t *)word_addr;
        uint8_t *bytes     = (uint8_t *)&existing;
        bytes[offset]      = *data;

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, word_addr, existing) != HAL_OK) {
            HAL_FLASH_Lock();
            __enable_irq();
            return false;
        }
        addr++; data++; len--;
        IWDG->KR = 0xAAAA;
    }

    HAL_FLASH_Lock();
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

