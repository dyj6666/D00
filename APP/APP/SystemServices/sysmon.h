/* ================================================================
 * sysmon —— 系统监控：堆/任务栈/看门狗状态汇总
 *
 * 架构位置：APP 服务层；sysmon 命令与 LCD 页共用
 * ================================================================ */
#ifndef SYSMON_H
#define SYSMON_H

/** 监控项采集函数：采集并直接打印到当前终端 */
typedef void (*sysmon_item_func)(void);

/** 注册一个监控项（应用/服务模块在各自 Init 中调用，打印时按注册顺序执行） */
int SysMon_RegisterItem(const char *name, sysmon_item_func print);

void SysMon_Init(void);

#endif
