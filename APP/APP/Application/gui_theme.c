/* ================================================================
 * gui_theme —— 主题实现：色板 + 通用组件工厂
 *
 * 架构位置：APP 应用层；所有界面组件统一样式来源
 * ================================================================ */
#include "gui_theme.h"

#include <string.h>

/* ---------------- 状态 → 颜色映射 ---------------- */
static lv_color_t state_color(gui_state_t st)
{
    switch (st) {
    case GUI_STATE_OK:      return GUI_COL_OK;
    case GUI_STATE_WARN:    return GUI_COL_WARN;
    case GUI_STATE_ERR:     return GUI_COL_ERR;
    default:                return GUI_COL_TEXT_DIM;
    }
}

/* ---------------- 卡片容器 ---------------- */
lv_obj_t *GuiTheme_Card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);          /* 清默认主题样式，全自定义 */
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, GUI_COL_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, GUI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
    /* 卡片不滚动（内容自控），避免误触滚动条 */
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

/* ---------------- 状态点 ---------------- */
lv_obj_t *GuiTheme_Dot(lv_obj_t *parent)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, GUI_COL_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    return dot;
}

void GuiTheme_DotSet(lv_obj_t *dot, gui_state_t st)
{
    if (dot == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(dot, state_color(st), LV_PART_MAIN);
}

/* ---------------- 状态色条 ---------------- */
lv_obj_t *GuiTheme_Stripe(lv_obj_t *parent, lv_coord_t h)
{
    lv_obj_t *stripe = lv_obj_create(parent);
    lv_obj_remove_style_all(stripe);
    lv_obj_set_size(stripe, 4, h);
    lv_obj_set_style_radius(stripe, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(stripe, GUI_COL_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, LV_PART_MAIN);
    return stripe;
}

void GuiTheme_StripeSet(lv_obj_t *obj, gui_state_t st)
{
    if (obj == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(obj, state_color(st), LV_PART_MAIN);
}

/* ---------------- 文本标签 ---------------- */
lv_obj_t *GuiTheme_Label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *lb = lv_label_create(parent);
    lv_label_set_text(lb, text != NULL ? text : "");
    lv_obj_set_style_text_font(lb, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lb, color, LV_PART_MAIN);
    return lb;
}

/* ---------------- 状态判定 ---------------- */
gui_state_t GuiTheme_StateOf(int ok)
{
    return (ok != 0) ? GUI_STATE_OK : GUI_STATE_ERR;
}

gui_state_t GuiTheme_StateOfWarn(int ok, int warn_cond)
{
    if (ok == 0) {
        return GUI_STATE_ERR;
    }
    return (warn_cond != 0) ? GUI_STATE_WARN : GUI_STATE_OK;
}

/* ---------------- 标题栏 ---------------- */
lv_obj_t *GuiTheme_TitleBar(lv_obj_t *parent, const char *title,
                            const char *sub)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 240, 28);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, GUI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = GuiTheme_Label(bar, title, &lv_font_montserrat_16,
                                 GUI_COL_PRIMARY);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 10, 0);

    if (sub != NULL) {
        lv_obj_t *s = GuiTheme_Label(bar, sub, &lv_font_montserrat_12,
                                     GUI_COL_TEXT_DIM);
        lv_obj_align(s, LV_ALIGN_RIGHT_MID, -10, 0);
    }
    return t;
}
