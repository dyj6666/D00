/* ================================================================
 * app_footer —— APP 分区尾部有效性魔数 + 版本号（DAP 直烧即可启动）
 *
 * 架构位置：APP 配置层；链接到 RUN 分区最后 8 字节（0x0805FFF8）
 * 背景：BOOT 以 0x0805FFF8 == 0x4F54412E 判定 RUN 有效。OTA 升级时由
 *       BOOT 在提交阶段自行写入魔数与版本；本文件保证 Keil DAP 直烧
 *       同样直接有效，与 Script/append_app_magic.py 效果等价（幂等）。
 * 注意：版本号须与 config/version.json 的 ota_version 保持一致。
 * ================================================================ */
#include <stdint.h>

#define APP_VALID_MAGIC     0x4F54412Eu   /* "OTA."：BOOT 判定 RUN 有效 */
#define APP_FOOTER_VERSION  198u          /* 与 config/version.json ota_version 同步 */

__attribute__((section(".ota_footer"), used))
const uint32_t ota_footer[2] = { APP_VALID_MAGIC, APP_FOOTER_VERSION };
