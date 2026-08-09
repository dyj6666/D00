#ifndef BSP_NVM_H
#define BSP_NVM_H

#include <stdint.h>

/* ================================================================
 * PARAM 区（扇区 11，128KB @ 0x080E0000）flash 访问抽象。
 * 仅供 SystemServices 配置持久化使用（串行化 + 地址越界保护）。
 * 注意：EraseParamSector 覆盖整个扇区（含 BOOT 参数双槽），
 *       调用方必须先读回保留内容，擦除后原样重写。
 * ================================================================ */

void BSP_NVM_Init(void);

/* 擦除 PARAM 扇区；返回 0=成功，负=失败 */
int  BSP_NVM_EraseParamSector(void);

/* 编程 words 个字（4B/字），须落在 PARAM 区内；返回 0=成功 */
int  BSP_NVM_ProgramWords(uint32_t addr, const uint32_t *data, uint32_t words);

/* 直接读取（flash 按地址访问） */
void BSP_NVM_ReadWords(uint32_t addr, uint32_t *out, uint32_t words);

#endif
