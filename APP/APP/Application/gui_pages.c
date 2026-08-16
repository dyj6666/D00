/* ================================================================
 * gui_pages —— 页面实现：主页仪表盘 / 网络监控 / 系统监控
 *
 * 布局（240×320 竖屏）：
 *   0-28    标题栏（页眉 + 状态副文本）
 *   28-288  页面内容（卡片/图表/列表）
 *   288-320 底部导航栏（HOME / NET / SYS）
 *
 * 刷新模型：GuiPages_Refresh 由 gui_task 每秒调用一次——
 *   ① 采集全部外设数据到本地快照（含 CPU 双采样差分）
 *   ② 更新文本/状态点/环形表/进度条
 *   ③ 吞吐曲线按 1s 追加一点（60 点 = 1 分钟窗口）
 * 页面对象常驻（构建一次），切换仅 lv_scr_load_anim，零重建开销。
 * ================================================================ */
#include "gui_pages.h"
#include "gui_theme.h"

#include "app_config.h"
#include "bsp_can.h"
#include "bsp_rtc.h"
#include "bsp_system.h"
#include "bsp_w25q128.h"
#include "data_link.h"
#include "err_mgr.h"
#include "eth_app.h"
#include "event_bus.h"
#include "icmp_svc.h"
#include "imu_svc.h"
#include "ota_agent.h"
#include "touch_svc.h"
#include "usr_store.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------------- 布局常量 ---------------- */
#define GUI_W              240
#define GUI_NAV_Y          288
#define GUI_NAV_H          32
#define GUI_CARD_W         108
#define GUI_CARD_H         42
#define GUI_CARD_GAP       8

/* 探测类操作（I2C/SPI）节流：5s 一次，避免频繁访问总线 */
#define GUI_PROBE_PERIOD   5u

/* ---------------- 数据快照（1s 采集，页面刷新只读快照） ---------------- */
typedef struct {
    /* ETH */
    uint8_t  eth_link;
    uint8_t  eth_ip[4];
    uint8_t  eth_gw[4];
    uint8_t  eth_mac[6];
    uint32_t eth_rx, eth_tx;        /* 累计帧数 */
    uint32_t eth_rx_rate, eth_tx_rate; /* 最近 1s 帧率 */
    uint32_t eth_uptime_s;
    uint8_t  eth_dhcp_on;
    /* ICMP */
    uint32_t icmp_rx, icmp_tx;
    uint32_t icmp_pps;
    uint32_t rtt_min, rtt_avg, rtt_max;
    /* CAN */
    uint8_t  can_active;
    uint32_t can_tx, can_rx, can_err, can_busoff, can_busload;
    /* IMU */
    uint8_t  imu_ready;
    float    imu_roll, imu_pitch, imu_temp;
    uint32_t imu_fault;
    /* TOUCH */
    uint8_t  touch_state;
    uint16_t touch_x, touch_y;
    /* 存储 */
    uint8_t  w25q_ok;
    uint8_t  eeprom_ok;
    uint32_t usr_used, usr_free, usr_keys;
    /* OTA */
    uint8_t  ota_state;
    uint32_t ota_received, ota_total;
    /* 系统 */
    uint32_t cpu_percent;
    uint32_t heap_free;             /* 字节 */
    uint32_t heap_total;            /* configTOTAL_HEAP_SIZE */
    uint32_t task_count;
    uint32_t uptime_s;
    uint32_t crash_seq;
    uint32_t eb_lost, eb_pool_free, eb_queue;
    uint32_t dl_cmd_lost, dl_tx_lost;
    /* RTC（0=无效） */
    uint8_t  rtc_valid;
    uint8_t  rtc_h, rtc_m, rtc_s;
} gui_data_t;

static gui_data_t s_data
    __attribute__((section(".ccmram"), zero_init));   /* 仅 GUI 任务访问，放 CCM 省主 RAM */

/* ---------------- 页面与控件句柄 ---------------- */
static lv_obj_t *s_scr_home, *s_scr_net, *s_scr_sys;

/* 主页 */
static lv_obj_t *s_h_sub;                 /* 标题栏副文本（时钟） */
static lv_obj_t *s_h_sum[3];              /* 摘要条：CPU/HEAP/UP */

/* 网络页 */
static lv_obj_t *s_n_link_dot, *s_n_link_ip, *s_n_gw, *s_n_mac, *s_n_dhcp;
static lv_obj_t *s_n_chart;
static lv_chart_series_t *s_n_ser_rx, *s_n_ser_tx;
static lv_obj_t *s_n_stat[8];
static lv_coord_t s_chart_ymax = 200;   /* 曲线 Y 轴自适应上限（无 getter，自跟踪） */

/* 系统页 */
static lv_obj_t *s_s_fw, *s_s_crash;
static lv_obj_t *s_s_cpu_arc, *s_s_cpu_pct;
static lv_obj_t *s_s_heap_bar, *s_s_heap_txt;
static lv_obj_t *s_s_rows[6];             /* 状态行文本 */

/* ---------------- CPU 占用：uxTaskGetSystemState 双采样差分 ----------------
 * 快照数组放 CCM：仅 CPU 访问（任务状态枚举），主 SRAM 让给 DMA/ETH */
#define GUI_CPU_SNAP_MAX   24
static TaskStatus_t s_cpu_snap[GUI_CPU_SNAP_MAX]
    __attribute__((section(".ccmram"), zero_init));
static uint32_t s_cpu_total_prev;
static uint32_t s_cpu_idle_prev;
static uint8_t  s_cpu_ready;

static void gui_cpu_sample(void)
{
    uint32_t total = 0;
    uint32_t n = uxTaskGetSystemState(s_cpu_snap, GUI_CPU_SNAP_MAX, &total);
    uint32_t idle = 0;
    for (uint32_t i = 0; i < n; i++) {
        /* 空闲任务名固定 "IDLE"（FreeRTOS 默认） */
        if (strcmp(s_cpu_snap[i].pcTaskName, "IDLE") == 0) {
            idle = s_cpu_snap[i].ulRunTimeCounter;
            break;
        }
    }
    if (s_cpu_ready && total > s_cpu_total_prev) {
        uint32_t dt = total - s_cpu_total_prev;
        uint32_t di = idle - s_cpu_idle_prev;
        s_data.cpu_percent = (dt > 0u) ? (100u - di * 100u / dt) : 0u;
        if (s_data.cpu_percent > 100u) {
            s_data.cpu_percent = 100u;   /* 首窗差分异常时钳位 */
        }
    } else {
        s_data.cpu_percent = 0u;         /* 首窗无基线，显示 0 */
    }
    s_cpu_total_prev = total;
    s_cpu_idle_prev = idle;
    s_cpu_ready = 1u;
}

/* ---------------- 探测节流计数 ---------------- */
static uint32_t s_probe_cnt;

/* ---------------- 数据采集（1s 一次，页面刷新只读快照） ---------------- */
static void gui_data_collect(void)
{
    /* ETH：先同步链路/IP/时长，再拷贝快照；帧率用 1s 差分 */
    EthApp_RefreshStatus();
    const eth_status_t *es = EthApp_GetStatus();
    uint32_t rx_prev = s_data.eth_rx, tx_prev = s_data.eth_tx;
    s_data.eth_link = es->link_up;
    memcpy(s_data.eth_ip, es->ip, 4);
    memcpy(s_data.eth_gw, es->gw, 4);
    memcpy(s_data.eth_mac, es->mac, 6);
    s_data.eth_rx = es->rx_packets;
    s_data.eth_tx = es->tx_packets;
    s_data.eth_uptime_s = es->link_uptime_s;
    s_data.eth_dhcp_on = EthApp_DhcpActive();
    s_data.eth_rx_rate = s_data.eth_rx - rx_prev;
    s_data.eth_tx_rate = s_data.eth_tx - tx_prev;

    /* ICMP */
    const icmp_svc_stat_t *is = IcmpSvc_GetStat();
    s_data.icmp_rx = is->echo_rx;
    s_data.icmp_tx = is->echo_tx;
    s_data.icmp_pps = is->rate_pps;
    s_data.rtt_min = is->rtt_count > 0u ? (uint32_t)(is->min_rtt_us / 1000u) : 0u;
    s_data.rtt_avg = is->rtt_count > 0u ? (uint32_t)(is->avg_rtt_us / 1000u) : 0u;
    s_data.rtt_max = is->rtt_count > 0u ? (uint32_t)(is->max_rtt_us / 1000u) : 0u;

    /* CAN */
    bsp_can_stats_t can;
    BSP_CAN_GetStats(&can);
    s_data.can_active = (uint8_t)BSP_CAN_IsActive();
    s_data.can_tx = can.tx_ok;
    s_data.can_rx = can.rx_ok;
    s_data.can_err = can.tx_err + can.rx_drop + can.rx_overrun;
    s_data.can_busoff = can.err_busoff;
    s_data.can_busload = can.bus_load_permille;

    /* IMU（struct 拷贝：volatile 字段按值取，保证单次快照一致） */
    const imu_svc_state_t *im = ImuSvc_GetState();
    s_data.imu_ready = im->ready;
    s_data.imu_roll = im->roll;
    s_data.imu_pitch = im->pitch;
    s_data.imu_temp = im->temp;
    s_data.imu_fault = im->fault_count;

    /* TOUCH */
    const touch_svc_state_t *ts = TouchSvc_GetState();
    s_data.touch_state = ts->state;
    s_data.touch_x = ts->x;
    s_data.touch_y = ts->y;

    /* 存储探测（5s 节流：I2C/SPI 访问有成本） */
    if (s_probe_cnt % GUI_PROBE_PERIOD == 0u) {
        s_data.w25q_ok = (BSP_W25Q128_Probe() == BSP_W25Q_OK) ? 1u : 0u;
        s_data.eeprom_ok = (UsrStore_Valid() > 0) ? 1u : 0u;
        UsrStore_Info(&s_data.usr_used, &s_data.usr_free);
        s_data.usr_keys = UsrStore_Count();
    }
    s_probe_cnt++;

    /* OTA */
    uint8_t st;
    uint32_t recv = 0, total = 0;
    Ota_Status(&st, &recv, &total);
    s_data.ota_state = st;
    s_data.ota_received = recv;
    s_data.ota_total = total;

    /* 系统 */
    s_data.task_count = (uint32_t)uxTaskGetNumberOfTasks();
    s_data.heap_free = (uint32_t)xPortGetFreeHeapSize();
    s_data.heap_total = (uint32_t)configTOTAL_HEAP_SIZE;
    s_data.uptime_s = BSP_GetTick() / 1000u;
    s_data.crash_seq = ERR_GetCrashSeq();
    s_data.eb_lost = EventBus_GetLostCount();
    s_data.eb_pool_free = EventBus_GetPoolFreeCount();
    s_data.eb_queue = EventBus_GetQueueCount();
    s_data.dl_cmd_lost = DataLink_GetCmdLostCount();
    s_data.dl_tx_lost = DataLink_GetTxLostCount();
    gui_cpu_sample();

    /* RTC（片上外设，直接读不阻塞；失败保持无效） */
    bsp_rtc_datetime_t dt;
    if (BSP_RTC_GetDateTime(&dt) == 0) {
        s_data.rtc_valid = 1u;
        s_data.rtc_h = dt.hours;
        s_data.rtc_m = dt.minutes;
        s_data.rtc_s = dt.seconds;
    } else {
        s_data.rtc_valid = 0u;
    }
}

/* ================================================================
 * 导航栏（三页共用：HOME / NET / SYS）
 * ================================================================ */
typedef struct {
    const char *label;
    void (*show)(void);
} nav_item_t;

static const nav_item_t s_nav_items[] = {
    { LV_SYMBOL_HOME " HOME",  GuiPages_ShowHome },
    { LV_SYMBOL_WIFI " NET",   GuiPages_ShowNet },
    { LV_SYMBOL_SETTINGS " SYS", GuiPages_ShowSys },
};

static void nav_click(lv_event_t *e)
{
    /* user_data 携带导航项索引（构建时绑定） */
    const nav_item_t *item = (const nav_item_t *)lv_event_get_user_data(e);
    if (item != NULL && item->show != NULL) {
        item->show();
    }
}

static void nav_build(lv_obj_t *parent)
{
    for (uint32_t i = 0; i < 3u; i++) {
        lv_obj_t *btn = lv_btn_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 76, GUI_NAV_H);
        lv_obj_set_pos(btn, (lv_coord_t)(8 + (lv_coord_t)i * 78), GUI_NAV_Y);
        lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, GUI_COL_CARD, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, GUI_COL_BORDER, LV_PART_MAIN);
        /* 按压态高亮，提供触摸反馈 */
        lv_obj_set_style_bg_color(btn, GUI_COL_CARD_HI, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, GUI_COL_PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);

        lv_obj_t *lb = lv_label_create(btn);
        lv_label_set_text(lb, s_nav_items[i].label);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(lb, GUI_COL_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_color(lb, GUI_COL_PRIMARY, LV_STATE_PRESSED);
        lv_obj_center(lb);

        lv_obj_add_event_cb(btn, nav_click, LV_EVENT_CLICKED,
                            (void *)&s_nav_items[i]);
    }
}

/* ================================================================
 * 外设卡片（主页 2×3 网格）：左侧状态色条 + 名称 + 状态点 + 数值行
 * ================================================================ */
typedef struct {
    const char *name;
    lv_obj_t   *stripe;
    lv_obj_t   *dot;
    lv_obj_t   *val;
} peri_card_t;

static peri_card_t s_cards[6];

static void card_build(lv_obj_t *parent, uint32_t idx,
                       const char *name, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *card = GuiTheme_Card(parent, GUI_CARD_W, GUI_CARD_H);
    lv_obj_set_pos(card, x, y);

    s_cards[idx].name = name;
    s_cards[idx].stripe = GuiTheme_Stripe(card, GUI_CARD_H);
    lv_obj_set_pos(s_cards[idx].stripe, 0, 0);

    lv_obj_t *nm = GuiTheme_Label(card, name, &lv_font_montserrat_12,
                                  GUI_COL_TEXT_DIM);
    lv_obj_set_pos(nm, 12, 4);

    s_cards[idx].dot = GuiTheme_Dot(card);
    lv_obj_align(s_cards[idx].dot, LV_ALIGN_TOP_RIGHT, -8, 7);

    s_cards[idx].val = GuiTheme_Label(card, "--", &lv_font_montserrat_14,
                                      GUI_COL_TEXT);
    lv_obj_set_pos(s_cards[idx].val, 12, 20);
    lv_label_set_long_mode(s_cards[idx].val, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_cards[idx].val, GUI_CARD_W - 18);
}

static void card_set(uint32_t idx, gui_state_t st, const char *fmt, ...)
{
    if (idx >= 6u || s_cards[idx].val == NULL) {
        return;
    }
    GuiTheme_StripeSet(s_cards[idx].stripe, st);
    GuiTheme_DotSet(s_cards[idx].dot, st);
    char buf[40];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(s_cards[idx].val, buf);
}

/* ================================================================
 * 主页：摘要条 + 外设网格
 * ================================================================ */
static void page_home_build(void)
{
    s_scr_home = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_home, GUI_COL_BG, 0);
    lv_obj_set_style_bg_opa(s_scr_home, LV_OPA_COVER, 0);

    GuiTheme_TitleBar(s_scr_home, "D00 Platform", NULL);
    s_h_sub = GuiTheme_Label(s_scr_home, "--:--:--",
                             &lv_font_montserrat_12, GUI_COL_ACCENT);
    lv_obj_align(s_h_sub, LV_ALIGN_TOP_RIGHT, -10, 6);

    /* 摘要条：CPU / HEAP / UPTIME */
    lv_obj_t *sum = GuiTheme_Card(s_scr_home, 224, 24);
    lv_obj_set_pos(sum, 8, 32);
    for (uint32_t i = 0; i < 3u; i++) {
        s_h_sum[i] = GuiTheme_Label(sum, "--", &lv_font_montserrat_12,
                                    GUI_COL_TEXT);
        lv_obj_align(s_h_sum[i], LV_ALIGN_LEFT_MID,
                     (lv_coord_t)(8 + (lv_coord_t)i * 74), 0);
    }

    /* 外设卡片 2×3 */
    static const char *names[6] = {
        "ETHERNET", "CAN BUS", "IMU", "TOUCH", "FLASH", "EEPROM",
    };
    for (uint32_t i = 0; i < 6u; i++) {
        lv_coord_t x = (lv_coord_t)(8 + (i % 2u) * (GUI_CARD_W + GUI_CARD_GAP));
        lv_coord_t y = (lv_coord_t)(62 + (i / 2u) * (GUI_CARD_H + GUI_CARD_GAP));
        card_build(s_scr_home, i, names[i], x, y);
    }

    nav_build(s_scr_home);
}

static void page_home_refresh(void)
{
    /* 摘要条 */
    lv_label_set_text_fmt(s_h_sum[0], LV_SYMBOL_BARS " CPU %lu%%",
                          (unsigned long)s_data.cpu_percent);
    lv_label_set_text_fmt(s_h_sum[1], LV_SYMBOL_DRIVE " HEAP %lu%%",
                          (unsigned long)(s_data.heap_total > 0u
                              ? s_data.heap_free * 100u / s_data.heap_total
                              : 0u));
    uint32_t up = s_data.uptime_s;
    lv_label_set_text_fmt(s_h_sum[2], LV_SYMBOL_REFRESH " %lu:%02lu:%02lu",
                          (unsigned long)(up / 3600u),
                          (unsigned long)((up % 3600u) / 60u),
                          (unsigned long)(up % 60u));

    /* 时钟（标题栏副文本） */
    if (s_data.rtc_valid) {
        lv_label_set_text_fmt(s_h_sub, "%02u:%02u:%02u",
                              (unsigned)s_data.rtc_h,
                              (unsigned)s_data.rtc_m,
                              (unsigned)s_data.rtc_s);
    }

    /* 外设卡片 */
    if (s_data.eth_link) {
        card_set(0, GUI_STATE_OK, "%u.%u.%u.%u",
                 s_data.eth_ip[0], s_data.eth_ip[1],
                 s_data.eth_ip[2], s_data.eth_ip[3]);
    } else {
        card_set(0, GUI_STATE_ERR, "link down");
    }

    if (s_data.can_active) {
        card_set(1, (s_data.can_err == 0u) ? GUI_STATE_OK : GUI_STATE_WARN,
                 "tx%lu rx%lu", (unsigned long)s_data.can_tx,
                 (unsigned long)s_data.can_rx);
    } else {
        card_set(1, GUI_STATE_ERR, "offline");
    }

    if (s_data.imu_ready) {
        char r[16], p[16];
        ImuSvc_FormatFixed(s_data.imu_roll, 1, r);
        ImuSvc_FormatFixed(s_data.imu_pitch, 1, p);
        card_set(2, (s_data.imu_fault == 0u) ? GUI_STATE_OK : GUI_STATE_WARN,
                 "R%s P%s", r, p);
    } else {
        card_set(2, GUI_STATE_ERR, "offline");
    }

    if (s_data.touch_state == TOUCH_EVT_DOWN ||
        s_data.touch_state == TOUCH_EVT_MOVE) {
        card_set(3, GUI_STATE_OK, "%u,%u",
                 (unsigned)s_data.touch_x, (unsigned)s_data.touch_y);
    } else {
        card_set(3, GUI_STATE_OK, "ready");
    }

    if (s_data.w25q_ok) {
        card_set(4, GUI_STATE_OK, "16MB");
    } else {
        card_set(4, GUI_STATE_ERR, "missing");
    }

    if (s_data.eeprom_ok) {
        card_set(5, GUI_STATE_OK, "%lu keys",
                 (unsigned long)s_data.usr_keys);
    } else {
        card_set(5, GUI_STATE_ERR, "missing");
    }
}

/* ================================================================
 * 网络页：链路信息 + 实时吞吐曲线 + ICMP 统计
 * ================================================================ */
#define GUI_CHART_POINTS   60

static void page_net_build(void)
{
    s_scr_net = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_net, GUI_COL_BG, 0);
    lv_obj_set_style_bg_opa(s_scr_net, LV_OPA_COVER, 0);

    GuiTheme_TitleBar(s_scr_net, "Network", "monitor");

    /* 链路信息卡 */
    lv_obj_t *link = GuiTheme_Card(s_scr_net, 224, 62);
    lv_obj_set_pos(link, 8, 32);

    lv_obj_t *lk = GuiTheme_Label(link, "LINK", &lv_font_montserrat_12,
                                  GUI_COL_TEXT_DIM);
    lv_obj_set_pos(lk, 10, 4);
    s_n_link_dot = GuiTheme_Dot(link);
    lv_obj_set_pos(s_n_link_dot, 48, 7);
    s_n_link_ip = GuiTheme_Label(link, "--", &lv_font_montserrat_14,
                                 GUI_COL_TEXT);
    lv_obj_align(s_n_link_ip, LV_ALIGN_TOP_RIGHT, -10, 0);

    s_n_gw = GuiTheme_Label(link, "GW --", &lv_font_montserrat_12,
                            GUI_COL_TEXT_DIM);
    lv_obj_set_pos(s_n_gw, 10, 22);
    s_n_mac = GuiTheme_Label(link, "MAC --", &lv_font_montserrat_12,
                             GUI_COL_TEXT_DIM);
    lv_obj_set_pos(s_n_mac, 10, 40);
    s_n_dhcp = GuiTheme_Label(link, "DHCP off", &lv_font_montserrat_12,
                              GUI_COL_ACCENT);
    lv_obj_align(s_n_dhcp, LV_ALIGN_TOP_RIGHT, -10, 40);

    /* 吞吐曲线卡 */
    lv_obj_t *ch = GuiTheme_Card(s_scr_net, 224, 92);
    lv_obj_set_pos(ch, 8, 102);
    lv_obj_t *ct = GuiTheme_Label(ch, "RATE  (pkts/s)", &lv_font_montserrat_12,
                                  GUI_COL_TEXT_DIM);
    lv_obj_set_pos(ct, 10, 4);

    s_n_chart = lv_chart_create(ch);
    lv_obj_set_size(s_n_chart, 208, 58);
    lv_obj_set_pos(s_n_chart, 8, 22);
    lv_obj_remove_style_all(s_n_chart);
    lv_obj_set_style_bg_color(s_n_chart, GUI_COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_n_chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_n_chart, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_n_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_n_chart, GUI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_n_chart, 4, LV_PART_MAIN);

    lv_chart_set_type(s_n_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_n_chart, GUI_CHART_POINTS);
    lv_chart_set_range(s_n_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 200);
    lv_chart_set_update_mode(s_n_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_line_width(s_n_chart, 1, LV_PART_MAIN);      /* 网格线 */
    lv_obj_set_style_line_color(s_n_chart, GUI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_size(s_n_chart, 0, LV_PART_INDICATOR);       /* 无数据点 */
    lv_obj_set_style_line_width(s_n_chart, 2, LV_PART_ITEMS);     /* 曲线线宽 */

    /* 系列颜色由 add_series 指定（lv_chart 渲染取 series->color），
     * RX 主色蓝 / TX 强调青，两条曲线同图区分 */
    s_n_ser_rx = lv_chart_add_series(s_n_chart, GUI_COL_PRIMARY,
                                     LV_CHART_AXIS_PRIMARY_Y);
    s_n_ser_tx = lv_chart_add_series(s_n_chart, GUI_COL_ACCENT,
                                     LV_CHART_AXIS_PRIMARY_Y);

    /* 统计网格 2×4 */
    static const char *stat_names[8] = {
        "RX pkts", "TX pkts", "ICMP rx", "ICMP tx",
        "RTT min", "RTT avg", "RTT max", "ICMP pps",
    };
    for (uint32_t i = 0; i < 8u; i++) {
        lv_obj_t *c = GuiTheme_Card(s_scr_net, 108, 28);
        lv_obj_set_pos(c, (lv_coord_t)(8 + (i % 2u) * 116),
                       (lv_coord_t)(202 + (i / 2u) * 32));
        GuiTheme_Label(c, stat_names[i], &lv_font_montserrat_12,
                       GUI_COL_TEXT_DIM);
        lv_obj_align(lv_obj_get_child(c, lv_obj_get_child_cnt(c) - 1u),
                     LV_ALIGN_TOP_LEFT, 10, 2);
        s_n_stat[i] = GuiTheme_Label(c, "--", &lv_font_montserrat_14,
                                     GUI_COL_TEXT);
        lv_obj_align(s_n_stat[i], LV_ALIGN_TOP_RIGHT, -10, 2);
    }

    nav_build(s_scr_net);
}

static void page_net_refresh(void)
{
    /* 链路状态 + IP */
    if (s_data.eth_link) {
        GuiTheme_DotSet(s_n_link_dot, GUI_STATE_OK);
        lv_label_set_text_fmt(s_n_link_ip, "%u.%u.%u.%u",
                              s_data.eth_ip[0], s_data.eth_ip[1],
                              s_data.eth_ip[2], s_data.eth_ip[3]);
        lv_label_set_text_fmt(s_n_gw, "GW %u.%u.%u.%u",
                              s_data.eth_gw[0], s_data.eth_gw[1],
                              s_data.eth_gw[2], s_data.eth_gw[3]);
        lv_label_set_text_fmt(s_n_mac, "MAC %02X:%02X:%02X:%02X:%02X:%02X",
                              s_data.eth_mac[0], s_data.eth_mac[1],
                              s_data.eth_mac[2], s_data.eth_mac[3],
                              s_data.eth_mac[4], s_data.eth_mac[5]);
    } else {
        GuiTheme_DotSet(s_n_link_dot, GUI_STATE_ERR);
        lv_label_set_text(s_n_link_ip, "--");
        lv_label_set_text(s_n_gw, "GW --");
        lv_label_set_text(s_n_mac, "MAC --");
    }
    lv_label_set_text(s_n_dhcp, s_data.eth_dhcp_on ? "DHCP on" : "DHCP off");

    /* 吞吐曲线（SHIFT 模式：追加一点自动滚动） */
    lv_chart_set_next_value(s_n_chart, s_n_ser_rx,
                            (lv_coord_t)s_data.eth_rx_rate);
    lv_chart_set_next_value(s_n_chart, s_n_ser_tx,
                            (lv_coord_t)s_data.eth_tx_rate);
    /* Y 轴自适应上限（200 起步，按峰值翻倍扩张，曲线不削顶） */
    uint32_t hi = (s_data.eth_rx_rate > s_data.eth_tx_rate)
                      ? s_data.eth_rx_rate : s_data.eth_tx_rate;
    if (hi > (uint32_t)s_chart_ymax && s_chart_ymax < 20000) {
        s_chart_ymax = (lv_coord_t)((uint32_t)s_chart_ymax * 2u);
        lv_chart_set_range(s_n_chart, LV_CHART_AXIS_PRIMARY_Y, 0,
                           s_chart_ymax);
    }

    /* 统计 */
    lv_label_set_text_fmt(s_n_stat[0], "%lu", (unsigned long)s_data.eth_rx);
    lv_label_set_text_fmt(s_n_stat[1], "%lu", (unsigned long)s_data.eth_tx);
    lv_label_set_text_fmt(s_n_stat[2], "%lu", (unsigned long)s_data.icmp_rx);
    lv_label_set_text_fmt(s_n_stat[3], "%lu", (unsigned long)s_data.icmp_tx);
    lv_label_set_text_fmt(s_n_stat[4], "%lu ms", (unsigned long)s_data.rtt_min);
    lv_label_set_text_fmt(s_n_stat[5], "%lu ms", (unsigned long)s_data.rtt_avg);
    lv_label_set_text_fmt(s_n_stat[6], "%lu ms", (unsigned long)s_data.rtt_max);
    lv_label_set_text_fmt(s_n_stat[7], "%lu", (unsigned long)s_data.icmp_pps);
}

/* ================================================================
 * 系统页：固件信息 + CPU 环形表 + 堆进度 + 服务状态行
 * ================================================================ */
static void page_sys_build(void)
{
    s_scr_sys = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_sys, GUI_COL_BG, 0);
    lv_obj_set_style_bg_opa(s_scr_sys, LV_OPA_COVER, 0);

    GuiTheme_TitleBar(s_scr_sys, "System", "monitor");

    /* 固件信息卡 */
    lv_obj_t *fw = GuiTheme_Card(s_scr_sys, 224, 50);
    lv_obj_set_pos(fw, 8, 32);
    s_s_fw = GuiTheme_Label(fw, "--", &lv_font_montserrat_14, GUI_COL_TEXT);
    lv_obj_set_pos(s_s_fw, 10, 6);
    s_s_crash = GuiTheme_Label(fw, "--", &lv_font_montserrat_12,
                               GUI_COL_TEXT_DIM);
    lv_obj_set_pos(s_s_crash, 10, 28);

    /* CPU 环形表 */
    lv_obj_t *cpu_c = GuiTheme_Card(s_scr_sys, 108, 100);
    lv_obj_set_pos(cpu_c, 8, 90);
    s_s_cpu_arc = lv_arc_create(cpu_c);
    lv_obj_set_size(s_s_cpu_arc, 72, 72);
    lv_obj_align(s_s_cpu_arc, LV_ALIGN_TOP_MID, 0, 6);
    lv_arc_set_rotation(s_s_cpu_arc, 270);
    lv_arc_set_bg_angles(s_s_cpu_arc, 0, 360);
    lv_arc_set_range(s_s_cpu_arc, 0, 100);
    lv_arc_set_value(s_s_cpu_arc, 0);
    lv_obj_remove_style(s_s_cpu_arc, NULL, LV_PART_KNOB);   /* 无旋钮 */
    lv_obj_clear_flag(s_s_cpu_arc, LV_OBJ_FLAG_CLICKABLE);  /* 只读 */
    lv_obj_set_style_arc_width(s_s_cpu_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_s_cpu_arc, GUI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_s_cpu_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_s_cpu_arc, GUI_COL_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_s_cpu_arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_s_cpu_arc, 0, LV_PART_INDICATOR);
    s_s_cpu_pct = GuiTheme_Label(cpu_c, "0%", &lv_font_montserrat_16,
                                 GUI_COL_TEXT);
    lv_obj_center(s_s_cpu_pct);
    GuiTheme_Label(cpu_c, "CPU", &lv_font_montserrat_12, GUI_COL_TEXT_DIM);
    lv_obj_align(lv_obj_get_child(cpu_c, lv_obj_get_child_cnt(cpu_c) - 1u),
                 LV_ALIGN_BOTTOM_MID, 0, -4);

    /* 堆进度卡 */
    lv_obj_t *heap_c = GuiTheme_Card(s_scr_sys, 108, 100);
    lv_obj_set_pos(heap_c, 124, 90);
    GuiTheme_Label(heap_c, "HEAP", &lv_font_montserrat_12, GUI_COL_TEXT_DIM);
    lv_obj_align(lv_obj_get_child(heap_c, lv_obj_get_child_cnt(heap_c) - 1u),
                 LV_ALIGN_TOP_LEFT, 10, 6);
    s_s_heap_bar = lv_bar_create(heap_c);
    lv_obj_set_size(s_s_heap_bar, 88, 10);
    lv_obj_align(s_s_heap_bar, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_color(s_s_heap_bar, GUI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_s_heap_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_s_heap_bar, GUI_COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_s_heap_bar, 5, LV_PART_INDICATOR);
    lv_bar_set_range(s_s_heap_bar, 0, 100);
    lv_bar_set_value(s_s_heap_bar, 0, LV_ANIM_OFF);
    s_s_heap_txt = GuiTheme_Label(heap_c, "--", &lv_font_montserrat_12,
                                  GUI_COL_TEXT);
    lv_obj_align(s_s_heap_txt, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* 服务状态行（多行文本，一次性刷新） */
    lv_obj_t *rows = GuiTheme_Card(s_scr_sys, 224, 88);
    lv_obj_set_pos(rows, 8, 198);
    for (uint32_t i = 0; i < 6u; i++) {
        s_s_rows[i] = GuiTheme_Label(rows, "--", &lv_font_montserrat_12,
                                     GUI_COL_TEXT_DIM);
        lv_obj_set_pos(s_s_rows[i], 10, (lv_coord_t)(4 + (lv_coord_t)i * 14));
    }

    nav_build(s_scr_sys);
}

static void page_sys_refresh(void)
{
    uint32_t ver = *(volatile uint32_t *)OTA_APP_VERSION_ADDR;
    lv_label_set_text_fmt(s_s_fw, "Firmware v%lu  Plan-B",
                          (unsigned long)ver);
    lv_label_set_text_fmt(s_s_crash, "Crash #%lu   Tasks %lu",
                          (unsigned long)s_data.crash_seq,
                          (unsigned long)s_data.task_count);

    /* CPU 环形表 */
    lv_arc_set_value(s_s_cpu_arc, (lv_coord_t)s_data.cpu_percent);
    lv_label_set_text_fmt(s_s_cpu_pct, "%lu%%",
                          (unsigned long)s_data.cpu_percent);

    /* 堆 */
    uint32_t used = (s_data.heap_total > s_data.heap_free)
                        ? (s_data.heap_total - s_data.heap_free) : 0u;
    uint32_t pct = (s_data.heap_total > 0u)
                       ? (used * 100u / s_data.heap_total) : 0u;
    lv_bar_set_value(s_s_heap_bar, (lv_coord_t)pct, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_s_heap_txt, "%lu/%lu KB",
                          (unsigned long)(used / 1024u),
                          (unsigned long)(s_data.heap_total / 1024u));

    /* 状态行 */
    lv_label_set_text_fmt(s_s_rows[0], LV_SYMBOL_BELL " EBus  pool %lu  q %lu  lost %lu",
                          (unsigned long)s_data.eb_pool_free,
                          (unsigned long)s_data.eb_queue,
                          (unsigned long)s_data.eb_lost);
    lv_label_set_text_fmt(s_s_rows[1], LV_SYMBOL_LIST " DLink cmd %lu  tx %lu",
                          (unsigned long)s_data.dl_cmd_lost,
                          (unsigned long)s_data.dl_tx_lost);
    lv_label_set_text_fmt(s_s_rows[2], LV_SYMBOL_SD_CARD " USR %luB/%luB  %lu keys",
                          (unsigned long)s_data.usr_used,
                          (unsigned long)s_data.usr_free,
                          (unsigned long)s_data.usr_keys);
    if (s_data.ota_state == OTA_ST_IDLE) {
        lv_label_set_text(s_s_rows[3], LV_SYMBOL_DOWNLOAD " OTA idle");
    } else {
        uint32_t p = (s_data.ota_total > 0u)
                         ? (s_data.ota_received * 100u / s_data.ota_total) : 0u;
        lv_label_set_text_fmt(s_s_rows[3], LV_SYMBOL_DOWNLOAD " OTA %lu%%  %lu/%lu",
                              (unsigned long)p,
                              (unsigned long)s_data.ota_received,
                              (unsigned long)s_data.ota_total);
    }
    lv_label_set_text_fmt(s_s_rows[4], LV_SYMBOL_WIFI " ETH %s  %lu/%lu pkt",
                          s_data.eth_link ? "UP" : "DOWN",
                          (unsigned long)s_data.eth_rx,
                          (unsigned long)s_data.eth_tx);
    if (s_data.rtc_valid) {
        lv_label_set_text_fmt(s_s_rows[5], "RTC %02u:%02u:%02u",
                              (unsigned)s_data.rtc_h,
                              (unsigned)s_data.rtc_m,
                              (unsigned)s_data.rtc_s);
    } else {
        lv_label_set_text(s_s_rows[5], "RTC n/a");
    }
}

/* ================================================================
 * 页面切换（方向动画：向右导航 MOVE_LEFT，向左 MOVE_RIGHT）
 * ================================================================ */
static lv_obj_t *s_active;

static void page_show(lv_obj_t *scr, lv_scr_load_anim_t dir)
{
    if (scr == s_active || scr == NULL) {
        return;
    }
    s_active = scr;
    lv_scr_load_anim(scr, dir, 150, 0, false);
}

void GuiPages_ShowHome(void) { page_show(s_scr_home, LV_SCR_LOAD_ANIM_MOVE_RIGHT); }
void GuiPages_ShowNet(void)  { page_show(s_scr_net,  LV_SCR_LOAD_ANIM_MOVE_LEFT); }
void GuiPages_ShowSys(void)  { page_show(s_scr_sys,  LV_SCR_LOAD_ANIM_MOVE_LEFT); }

lv_obj_t *GuiPages_GetHome(void)
{
    return s_scr_home;
}

/* ---------------- 初始化与周期刷新 ---------------- */
void GuiPages_Init(void)
{
    memset(&s_data, 0, sizeof(s_data));
    page_home_build();
    page_net_build();
    page_sys_build();
    s_active = s_scr_home;
    lv_scr_load(s_scr_home);
    gui_data_collect();   /* 首窗立即采集（CPU 基线等） */
}

void GuiPages_Refresh(void)
{
    gui_data_collect();
    page_home_refresh();
    page_net_refresh();
    page_sys_refresh();
}
