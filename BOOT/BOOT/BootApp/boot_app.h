/* BOOT 应用层：启动流程、APP 校验、跳转与 OTA 升级状态机 */
#ifndef BOOT_APP_H
#define BOOT_APP_H

/* 运行 BOOT 主流程：初始化 → 检查升级标志 → 校验 APP → 跳转/进入升级模式 */
void BootApp_Run(void);

#endif
