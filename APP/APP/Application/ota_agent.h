/* ================================================================
 * ota_agent —— 运行时 OTA 下载核心接口（传输无关）
 *
 * 架构位置：APP 应用层；UART/TCP/HTTP 传输统一调用本接口
 * 核心流程：Ota_Begin -> Ota_Data(240B/块) -> Ota_End -> BOOT 切换
 * ================================================================ */
#ifndef OTA_AGENT_H
#define OTA_AGENT_H

#include <stdint.h>

/* OTA 状态机：IDLE=空闲，RECEIVING=接收中，DONE=已收齐待切换 */
#define OTA_ST_IDLE         0
#define OTA_ST_RECEIVING    1
#define OTA_ST_DONE         2

/** @brief OTA Agent 初始化：订阅命令、启动确认、打印参数区状态 */
void    OtaAgent_Init(void);

/**
 * @brief  开始 OTA 下载会话（传输层统一入口）
 * @return 0=成功；1=已在接收；2=长度非法；3=擦除失败；4=版本降级拒绝
 */
uint8_t Ota_Begin(uint32_t version, uint32_t size);

/**
 * @brief  顺序写入一块固件（offset 必须连续）
 * @return 0=成功；1=非接收态；2=参数非法；3=Flash 写失败
 */
uint8_t Ota_Data(uint32_t offset, const uint8_t *data, uint16_t len);

/** @brief 结束下载并触发 BOOT 切换；0=成功（随后复位） */
uint8_t Ota_End(void);

/** @brief 读取状态与进度（state/received/total 输出参数） */
uint8_t Ota_Status(uint8_t *state, uint32_t *received, uint32_t *total);

/** @brief 强制回到 IDLE 并清空会话槽（配合 CLI --no-resume） */
uint8_t Ota_Reset(void);

/** @brief 危险自测：参数区置 PENDING+MAX，复位后触发 BOOT 回滚 */
void Ota_ForceRollbackTest(void);

#endif /* OTA_AGENT_H */
