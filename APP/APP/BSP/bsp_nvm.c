/* ================================================================
 * PARAM 区 flash 访问实现（扇区 11 @ 0x080E0000，128KB）
 *   - 任务级互斥串行化（OS 互斥量），中断不触碰 flash
 *   - 擦除/编程期间保持中断使能：SysTick 仍喂 IWDG（擦除约 1-2s）
 * ================================================================ */
#include "bsp_nvm.h"
#include "app_config.h"
#include "main.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"

#define NVM_PARAM_BASE      OTA_PARAM_ADDR            /* 0x080E0000 */
#define NVM_PARAM_SIZE      (128u * 1024u)
#define NVM_PARAM_SECTOR    FLASH_SECTOR_11

static osMutexId_t s_flash_mutex = NULL;

void BSP_NVM_Init(void)
{
    if (s_flash_mutex == NULL) {
        s_flash_mutex = osMutexNew(NULL);
    }
}

static void nvm_flag_clear(void)
{
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
}

static int nvm_lock(void)
{
    if (s_flash_mutex == NULL) {
        return -1;
    }
    return (osMutexAcquire(s_flash_mutex, osWaitForever) == osOK) ? 0 : -1;
}

static void nvm_unlock(void)
{
    if (s_flash_mutex != NULL) {
        osMutexRelease(s_flash_mutex);
    }
}

int BSP_NVM_EraseParamSector(void)
{
    if (nvm_lock() != 0) {
        return -1;
    }
    int ret = 0;
    HAL_FLASH_Unlock();
    nvm_flag_clear();
    FLASH_EraseInitTypeDef er;
    er.TypeErase = FLASH_TYPEERASE_SECTORS;
    er.Sector = NVM_PARAM_SECTOR;
    er.NbSectors = 1;
    er.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    uint32_t err = 0;
    if (HAL_FLASHEx_Erase(&er, &err) != HAL_OK || err != 0xFFFFFFFFu) {
        ret = -2;
    }
    HAL_FLASH_Lock();
    nvm_unlock();
    return ret;
}

int BSP_NVM_ProgramWords(uint32_t addr, const uint32_t *data, uint32_t words)
{
    if (addr < NVM_PARAM_BASE ||
        addr + words * 4u > NVM_PARAM_BASE + NVM_PARAM_SIZE ||
        (addr & 0x3u) != 0) {
        return -3;                       /* 越界/未对齐 */
    }
    if (nvm_lock() != 0) {
        return -1;
    }
    int ret = 0;
    HAL_FLASH_Unlock();
    nvm_flag_clear();
    for (uint32_t i = 0; i < words; i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i * 4u,
                              data[i]) != HAL_OK) {
            ret = -2;
            break;
        }
    }
    HAL_FLASH_Lock();
    nvm_unlock();
    return ret;
}

void BSP_NVM_ReadWords(uint32_t addr, uint32_t *out, uint32_t words)
{
    const volatile uint32_t *p = (const volatile uint32_t *)addr;
    for (uint32_t i = 0; i < words; i++) {
        out[i] = p[i];
    }
}
