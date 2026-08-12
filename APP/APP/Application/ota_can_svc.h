/* ================================================================
 * ota_can_svc —— CAN 总线 OTA 服务（多协议 OTA 第 4 通道）
 *
 * 架构位置：APP 应用层；经 Ota_Begin/Data/End 与下载核心解耦
 *
 * 协议（见 Config/can_proto.h，与 HOST ota_can_cli.py 对称）：
 *   控制 0x200：BEGIN(version+size) / END / STATUS / ABORT；
 *   数据 0x201：行帧规约，每 240B 一组 → Ota_Data 写下载区；
 *   应答 0x210：BEGIN_OK/ERR、END_RESULT、STATUS、ABORT 回执。
 * ================================================================ */
#ifndef OTA_CAN_SVC_H
#define OTA_CAN_SVC_H

/** @brief 初始化 CAN OTA：注册传输位 + RX 回调 + 超时监管任务 */
void OtaCanSvc_Init(void);

#endif /* OTA_CAN_SVC_H */
