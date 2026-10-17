/* ================================================================
 * name_card.h —— 开机名字卡（在 LCD 上显示汉字）
 * ================================================================ */
#ifndef __NAME_CARD_H
#define __NAME_CARD_H

#include "esp_err.h"

/* 显示内容：要改名字请同步修改 tools/gen_font.py 里的 CHARS 并重新生成字模 */
#define NAME_CARD_TEXT      "董衍俊"

/**
 * @brief  在 LCD 上居中绘制名字卡（白底 + 深蓝字，抗锯齿）
 * @return ESP_OK 成功；其他为失败原因
 */
esp_err_t name_card_show(void);

#endif
