/* ================================================================
 * bsp_flash —— 片内 Flash 擦写封装实现
 *
 * 覆盖 STM32F407 全片（0x0800_0000~0x080F_FFFF）扇区映射；逐字编程
 * 短暂关中断（CAN 1Mbps 连续帧下 FIFO 可在两字之间排空）。
 * ================================================================ */
#include "bsp_flash.h"
#include "stm32f4xx_hal.h"

#include <string.h>

/** @brief Flash 绝对地址 -> 扇区号（0x0800_0000~0x080F_FFFF） */
static uint32_t flash_sector_of(uint32_t addr)
{
    if (addr >= 0x08000000 && addr < 0x08004000) return FLASH_SECTOR_0;
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

bool BSP_Flash_EraseRange(uint32_t addr, uint32_t len)
{
    uint32_t start_sector = flash_sector_of(addr);
    uint32_t end_sector = (len == 0) ? start_sector
                                     : flash_sector_of(addr + len - 1);
    for (uint32_t sector = start_sector; sector <= end_sector; sector++) {
        HAL_FLASH_Unlock();
        /* 清全部错误标志（含 OPTERR/SOP）：RDP 解除或此前操作可能残留，
         * HAL_FLASHEx_Erase 检测到错误会直接返回 HAL_ERROR */
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_PGSERR | FLASH_FLAG_PGPERR |
                               FLASH_FLAG_PGAERR | FLASH_FLAG_WRPERR |
                               FLASH_FLAG_OPERR);
        FLASH_EraseInitTypeDef er = {
            .TypeErase = FLASH_TYPEERASE_SECTORS,
            .Sector = sector,
            .NbSectors = 1,
            .VoltageRange = FLASH_VOLTAGE_RANGE_3,
        };
        uint32_t err = 0;
        HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&er, &err);
        HAL_FLASH_Lock();
        if (st != HAL_OK || err != 0xFFFFFFFF) {
            return false;
        }
    }
    return true;
}

bool BSP_Flash_ProgramWord(uint32_t addr, uint32_t val)
{
    __disable_irq();   /* 仅保护单字编程：防止与其它 Flash 访问交错 */
    HAL_StatusTypeDef hs = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, val);
    __enable_irq();
    return (hs == HAL_OK);
}

bool BSP_Flash_Write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (len == 0) return true;
    HAL_FLASH_Unlock();

    /* 前导非对齐字节 */
    while ((addr & 0x03) && len > 0) {
        uint32_t wa = addr & ~0x03u;
        uint32_t val = *(volatile uint32_t *)wa;
        ((uint8_t *)&val)[addr & 0x03] = *data;
        if (!BSP_Flash_ProgramWord(wa, val)) {
            HAL_FLASH_Lock();
            return false;
        }
        addr++; data++; len--;
    }
    /* 整字对齐段 */
    while (len >= 4) {
        uint32_t word;
        memcpy(&word, data, 4);
        if (!BSP_Flash_ProgramWord(addr, word)) {
            HAL_FLASH_Lock();
            return false;
        }
        addr += 4; data += 4; len -= 4;
    }
    /* 尾部非对齐 */
    while (len > 0) {
        uint32_t wa = addr & ~0x03u;
        uint32_t val = *(volatile uint32_t *)wa;
        ((uint8_t *)&val)[addr & 0x03] = *data;
        if (!BSP_Flash_ProgramWord(wa, val)) {
            HAL_FLASH_Lock();
            return false;
        }
        addr++; data++; len--;
    }
    HAL_FLASH_Lock();
    return true;
}

void BSP_Flash_ResetController(void)
{
    /* 清全部错误标志（含 OPTERR/SOP）：RDP 解除或此前操作可能残留 */
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_PGSERR | FLASH_FLAG_PGPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_OPERR);
    HAL_FLASH_Unlock();
    HAL_FLASH_Lock();
}

uint32_t BSP_Flash_GetStatusSR(void)
{
    return (uint32_t)FLASH->SR;
}
