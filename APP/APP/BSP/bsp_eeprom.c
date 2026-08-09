/* ================================================================
 * AT24C02 驱动实现（I2C2 / 0x50，8B 页写，总线自恢复）
 * ================================================================ */
#include "bsp_eeprom.h"
#include "bsp_i2c.h"
#include "i2c.h"
#include "cmsis_os2.h"

#define EEPROM_I2C_TIMEOUT  100
#define EEPROM_WRITE_CYCLE  5      /* AT24C02 写周期 ≤5ms */

static void eeprom_bus_recover(void)
{
    HAL_I2C_DeInit(&hi2c2);
    HAL_I2C_Init(&hi2c2);
}

int BSP_EEPROM_Probe(void)
{
    int ok = -1;
    if (BSP_I2C2_Lock(50) == 0) {
        uint8_t d = 0;
        if (HAL_I2C_Mem_Read(&hi2c2, BSP_EEPROM_ADDR, 0, I2C_MEMADD_SIZE_8BIT,
                             &d, 1, EEPROM_I2C_TIMEOUT) == HAL_OK) {
            ok = 0;
        }
        BSP_I2C2_Unlock();
    }
    return ok;
}

int BSP_EEPROM_Init(void)
{
    BSP_I2C1_Init();
    BSP_I2C2_Init();
    return BSP_EEPROM_Probe();
}

int BSP_EEPROM_Read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    if (buf == NULL || addr + len > BSP_EEPROM_SIZE) {
        return -1;
    }
    int ret = -1;
    if (BSP_I2C2_Lock(100) == 0) {
        /* AT24C02 随机读支持跨页连续读（地址自动回绕） */
        if (HAL_I2C_Mem_Read(&hi2c2, BSP_EEPROM_ADDR, addr,
                             I2C_MEMADD_SIZE_8BIT, buf, len,
                             EEPROM_I2C_TIMEOUT) == HAL_OK) {
            ret = 0;
        } else {
            eeprom_bus_recover();
        }
        BSP_I2C2_Unlock();
    }
    return ret;
}

int BSP_EEPROM_Write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    if (buf == NULL || addr + len > BSP_EEPROM_SIZE) {
        return -1;
    }
    int ret = -1;
    if (BSP_I2C2_Lock(200) == 0) {
        uint16_t off = 0;
        while (off < len) {
            /* 页边界拆分：每页最多 8B */
            uint16_t page_left = BSP_EEPROM_PAGE -
                                 (uint16_t)((addr + off) % BSP_EEPROM_PAGE);
            uint16_t chunk = len - off;
            if (chunk > page_left) {
                chunk = page_left;
            }
            if (HAL_I2C_Mem_Write(&hi2c2, BSP_EEPROM_ADDR, addr + off,
                                  I2C_MEMADD_SIZE_8BIT,
                                  (uint8_t *)buf + off, chunk,
                                  EEPROM_I2C_TIMEOUT) != HAL_OK) {
                eeprom_bus_recover();
                break;
            }
            off += chunk;
            osDelay(EEPROM_WRITE_CYCLE);   /* 等待内部写周期 */
        }
        if (off == len) {
            ret = 0;
        }
        BSP_I2C2_Unlock();
    }
    return ret;
}
