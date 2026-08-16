/* ================================================================
 * gui_theme —— GUI 主题：色板与通用组件工厂
 *
 * 架构位置：APP 应用层；gui_pages 的视觉基础，集中管理配色与
 *           卡片/状态点/标签等基础控件的统一样式，保证全界面
 *           视觉一致（一处改色，全局生效）。
 * ================================================================ */
#ifndef GUI_THEME_H
#define GUI_THEME_H

#include "lvgl.h"

/* ---------------- 主题色板（深色优雅系） ----------------
 * 背景深蓝黑，卡片略亮，主色信息蓝，成功/警告/错误三态分明 */
#define GUI_COL_BG        lv_color_hex(0x0E1420)   /* 页面背景 */
#define GUI_COL_CARD      lv_color_hex(0x182030)   /* 卡片底 */
#define GUI_COL_CARD_HI   lv_color_hex(0x1F2B3D)   /* 卡片高亮底（按压/选中） */
#define GUI_COL_BORDER    lv_color_hex(0x2A3548)   /* 卡片描边 */
#define GUI_COL_PRIMARY   lv_color_hex(0x4FC3F7)   /* 主色：信息蓝 */
#define GUI_COL_ACCENT    lv_color_hex(0x4DB6AC)   /* 强调色：青 */
#define GUI_COL_OK        lv_color_hex(0x66BB6A)   /* 正常绿 */
#define GUI_COL_WARN      lv_color_hex(0xFFD54F)   /* 警告黄 */
#define GUI_COL_ERR       lv_color_hex(0xFF5252)   /* 错误红 */
#define GUI_COL_TEXT      lv_color_hex(0xECEFF1)   /* 主文本 */
#define GUI_COL_TEXT_DIM  lv_color_hex(0x8A97A8)   /* 次级文本 */

/* ---------------- 状态枚举（状态点/色条统一用） ---------------- */
typedef enum {
    GUI_STATE_UNKNOWN = 0,   /* 灰：未启用/未知 */
    GUI_STATE_OK      = 1,   /* 绿：正常 */
    GUI_STATE_WARN    = 2,   /* 黄：降级/警告 */
    GUI_STATE_ERR     = 3,   /* 红：故障 */
} gui_state_t;

/* ---------------- 通用组件工厂 ---------------- */

/* 卡片容器：圆角 + 深色底 + 细描边（无阴影，省渲染开销） */
lv_obj_t *GuiTheme_Card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h);

/* 状态点：8px 圆点，颜色随状态（GUI_STATE_*） */
lv_obj_t *GuiTheme_Dot(lv_obj_t *parent);
void      GuiTheme_DotSet(lv_obj_t *dot, gui_state_t st);

/* 状态色条：卡片左侧 4px 竖条，随状态变色（增强可读性） */
lv_obj_t *GuiTheme_Stripe(lv_obj_t *parent, lv_coord_t h);
void      GuiTheme_StripeSet(lv_obj_t *obj, gui_state_t st);

/* 文本标签（常用字体/颜色快捷方式） */
lv_obj_t *GuiTheme_Label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, lv_color_t color);

/* 状态色映射：布尔/错误码 → 状态枚举（供外设卡片统一判定） */
gui_state_t GuiTheme_StateOf(int ok);          /* ok!=0 → OK，否则 ERR */
gui_state_t GuiTheme_StateOfWarn(int ok, int warn_cond); /* warn_cond 时 WARN */

/* 标题栏：页眉（左侧标题 + 右侧副文本），返回标题 label */
lv_obj_t *GuiTheme_TitleBar(lv_obj_t *parent, const char *title,
                            const char *sub);

#endif /* GUI_THEME_H */
