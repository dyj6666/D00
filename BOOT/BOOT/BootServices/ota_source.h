/* ================================================================
 * ota_source —— 固件包读源抽象（方案B：单一外部 SPI Flash 源）
 *
 * 架构位置：BOOT BootServices 层；security / boot_app 只依赖本抽象，
 *           不感知物理介质。升级流程经 OtaSource_External 探测外部
 *           ota_dl 槽；YMODEM 兜底收包同样写入外部槽，读源保持唯一。
 * ================================================================ */
#ifndef OTA_SOURCE_H
#define OTA_SOURCE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t size;                 /* 包总长（头 + 密文 + 签名） */
    void (*read)(uint32_t off, void *buf, uint32_t len);   /* 读包任意偏移 */
} ota_source_t;

/* 外部源：探测外部 ota_dl 槽 0 有效包；true=就绪（含 size） */
bool OtaSource_External(ota_source_t *s);

#endif /* OTA_SOURCE_H */
