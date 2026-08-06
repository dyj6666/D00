/**
 * @file    boot_param.h
 * @brief   Flash 参数区管理：启动状态/回滚计数持久化（双份冗余 + CRC32）
 */
#ifndef BOOT_PARAM_H
#define BOOT_PARAM_H

#include <stdint.h>
#include <stdbool.h>
#include "boot_config.h"

void     boot_param_load(boot_param_t *out);               /* 读取（双份选优） */
bool     boot_param_save(const boot_param_t *in);          /* 写入（双份） */
void     boot_param_defaults(boot_param_t *p);             /* 默认值 NORMAL */
uint32_t boot_param_crc(const boot_param_t *p);            /* 结构 CRC32 */

#endif
