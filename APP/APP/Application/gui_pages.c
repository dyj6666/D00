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
#include "cam_link.h"
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
#include <math.h>   /* sinf（GIMBAL 演示轨迹） */

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
static lv_obj_t *s_scr_home, *s_scr_net, *s_scr_sys, *s_scr_cam, *s_scr_gimbal;

/* CAM 页（摄像头链路状态） */
static lv_obj_t *s_c_dot;                  /* 链路状态点 */
static lv_obj_t *s_c_frames, *s_c_err, *s_c_swipe, *s_c_last;  /* 统计 */
static lv_obj_t *s_c_hand, *s_c_pos, *s_c_size;
static lv_obj_t *s_c_gesture, *s_c_conf, *s_c_swl, *s_c_swr;

/* ================= GIMBAL 云台模型页（纯视觉演示，无算法） =================
 * 视场模拟（Lissajous 目标轨迹 + 准星跟随插值）+ PAN/TILT 弧形仪表。
 * 数据仅演示用途（demo 时间步进），后续接真实视觉/IMU/控制。
 * 性能设计：准星合并为单 lv_canvas（1 对象 vs 4 对象，失效区域收缩）；
 *           仪表全表重绘最贵 → 4 帧降频；标题栏实时显示 fps/渲染耗时。 */
static lv_obj_t *s_g_target, *s_g_core;        /* 目标块（apriltag 风格）+ 白心 */
static lv_obj_t *s_g_cross_canvas;             /* 准星画布（十字+圆环+中心点，单对象） */
/* 准星画布缓冲：主 SRAM 已满（L6406E .data 溢出），放 CCM（CPU-only 访问，
 * LVGL 软件绘制读缓冲安全；DMA 不可达但画布只被 CPU 读写） */
static lv_color_t s_g_cross_cbuf[30 * 30]
    __attribute__((section(".ccmram"), zero_init));
static lv_obj_t *s_g_pan_meter, *s_g_tilt_meter;   /* 双轴弧形仪表 */
static lv_meter_indicator_t *s_g_pan_ind, *s_g_tilt_ind; /* 仪表指针（直接持有） */
static lv_obj_t *s_g_pan_val, *s_g_tilt_val;   /* 仪表数值标签 */
static lv_obj_t *s_g_track_dot;                /* 跟踪状态点 */
static lv_obj_t *s_g_dx, *s_g_dy;              /* 偏差标签 */
static float s_g_demo_t;                       /* 演示时间（秒） */
static float s_g_follow_x, s_g_follow_y;       /* 准星跟随位置（插值） */
static lv_timer_t *s_g_anim_timer;             /* 动画定时器（33ms ≈ 30fps；保留句柄供后续 pause/resume） */
static lv_obj_t *s_g_perf;                     /* 标题栏性能显示（fps/渲染耗时） */
static uint8_t s_g_frame_cnt;                  /* 仪表降频计数器（每 6 帧） */
static uint32_t s_g_last_ms;                   /* 上一帧 lv_tick（fps 计算） */
/* 通道模式：0=DEMO（Lissajous 演示） 1=RAW（加速度原始） 2=COMP（互补）
 * 3=KF（卡尔曼） 4=PID（调参实验室） */
static uint8_t s_g_mode;
static lv_obj_t *s_g_ch_btn;                   /* 通道切换按钮（HUD 条右侧） */
static const char *const s_g_mode_name[5] = { "DEMO", "RAW", "COMP", "KF", "PID" };

/* ---- PID 调参实验室（HIL：真实算法 + 虚拟二阶对象 + 真实扰动） ---- */
#include "ctrl/ctrl.h"
static PID_Pos s_pid_lab;                      /* ctrl 库位置式 PID（全特性） */
static float s_plant_y, s_plant_ydot;          /* 虚拟云台状态（二阶对象） */
static float s_plant_u_d[3];                   /* 输出延迟缓冲（2 步 ≈ 66ms） */
static uint8_t s_pid_run_mode;                 /* 0=阶跃 1=正弦 2=扰动(MPU6050) */
static float s_pid_t;                          /* 实验室时间 */
#define PID_CV_PTS  40                          /* 曲线点数（40×3×4B=480B） */
static float s_cv_set[PID_CV_PTS], s_cv_y[PID_CV_PTS], s_cv_u[PID_CV_PTS];
static uint32_t s_cv_n;                         /* 已采点数 */
static uint32_t s_cv_tick;                      /* 曲线采样节拍（每 3 帧≈100ms） */
static lv_obj_t *s_g_curve_set, *s_g_curve_y, *s_g_curve_u;  /* 曲线线对象 */
static lv_point_t s_cv_pts_set[PID_CV_PTS], s_cv_pts_y[PID_CV_PTS],
                  s_cv_pts_u[PID_CV_PTS];
static lv_obj_t *s_g_kp_card, *s_g_ki_card, *s_g_kd_card;   /* 参数卡（PID 模式） */
static lv_obj_t *s_g_kp_val, *s_g_ki_val, *s_g_kd_val;
static lv_obj_t *s_g_pan_card, *s_g_tilt_card; /* 仪表卡（非 PID 模式显示） */
/* LVGL 单帧渲染耗时（gui_app.c 实测） */
extern uint32_t g_gui_render_us;

/* 主页 */
static lv_obj_t *s_h_sub;                 /* 标题栏副文本（时钟） */
static lv_obj_t *s_h_sum[3];              /* 摘要条：CPU/HEAP/UP */
static lv_obj_t *s_h_fw;                  /* 固件版本 + build（标题栏左下） */

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
    uint8_t idle_found = 0;
    for (uint32_t i = 0; i < n; i++) {
        /* 空闲任务名固定 "IDLE"（FreeRTOS 默认） */
        if (strcmp(s_cpu_snap[i].pcTaskName, "IDLE") == 0) {
            idle = s_cpu_snap[i].ulRunTimeCounter;
            idle_found = 1u;
            break;
        }
    }
    /* 差分窗口：cpu% = 100 - idle_delta/total_delta。
     * IDLE 未找到（任务表溢出）或首窗无基线时置 0（防御，不误报 100%）。 */
    if (s_cpu_ready && idle_found && total > s_cpu_total_prev) {
        uint32_t dt = total - s_cpu_total_prev;
        uint32_t di = idle - s_cpu_idle_prev;
        s_data.cpu_percent = (dt > 0u) ? (100u - di * 100u / dt) : 0u;
        if (s_data.cpu_percent > 100u) {
            s_data.cpu_percent = 100u;   /* 首窗差分异常时钳位 */
        }
    } else {
        s_data.cpu_percent = 0u;
    }
    s_cpu_total_prev = total;
    s_cpu_idle_prev = idle;
    s_cpu_ready = 1u;
}

/* ---------------- 探测节流计数 ---------------- */
static uint32_t s_probe_cnt;

/* ---------------- 数据采集（250ms 三相轮转的相 0 执行，只读快照） ---------------- */
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
    /* 采集粒度 250ms：差分 × 4 折算为帧/s（曲线 Y 轴单位） */
    s_data.eth_rx_rate = (s_data.eth_rx - rx_prev) * 4u;
    s_data.eth_tx_rate = (s_data.eth_tx - tx_prev) * 4u;

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
    { LV_SYMBOL_IMAGE " CAM",  GuiPages_ShowCam },
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
    const uint32_t n = (uint32_t)(sizeof(s_nav_items) / sizeof(s_nav_items[0]));
    const lv_coord_t bw = 52;                 /* 4 按钮适配 240 宽 */
    const lv_coord_t gap = 6;
    lv_coord_t x0 = 8;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *btn = lv_btn_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, bw, GUI_NAV_H);
        lv_obj_set_pos(btn, x0 + (lv_coord_t)i * (bw + gap), GUI_NAV_Y);
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
    /* 禁滚动：内容在导航栏(288)之内，超屏宁可裁剪——滚动拖动 = 大面积
     * 重绘 = 卡顿（见 13.4） */
    lv_obj_clear_flag(s_scr_home, LV_OBJ_FLAG_SCROLLABLE);

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

    /* 固件信息卡（卡片区下方，与摘要条同风格嵌入布局）：
     * 显示版本（0x080DFFFC）+ build（PARAM last_build_no），上电即见当前固件 */
    lv_obj_t *fwcard = GuiTheme_Card(s_scr_home, 224, 24);
    lv_obj_set_pos(fwcard, 8, 212);
    s_h_fw = GuiTheme_Label(fwcard, "FW --",
                            &lv_font_montserrat_12, GUI_COL_TEXT);
    lv_obj_align(s_h_fw, LV_ALIGN_LEFT_MID, 8, 0);

    nav_build(s_scr_home);
}

/* ---------------- 主页分片刷新（250ms 轮转相，彻底错峰） ----------------
 * 原整页 1s 全刷：10+ label 同帧重绘造成可感知顿挫；拆成
 * top(摘要+时钟) / cards0-2 / cards3-5 三片，每 250ms 一片，
 * 每片仅 3-4 个控件变化（LVGL 相同文本自动跳过重绘），刷新帧轻量。 */
static void page_home_refresh_top(void)
{
    /* 固件版本（0x080DFFFC）+ build（PARAM last_build_no 0x080E0014）：
     * build 全 0xFF（PARAM 未初始化/升级擦除）时显示 "--"（容错） */
    uint32_t fw_ver = *(volatile uint32_t *)0x080DFFFC;
    uint32_t fw_build = *(volatile uint32_t *)0x080E0014;
    if (fw_build == 0xFFFFFFFFu) {
        lv_label_set_text_fmt(s_h_fw, "FW v%lu b--",
                              (unsigned long)fw_ver);
    } else {
        lv_label_set_text_fmt(s_h_fw, "FW v%lu b%lu",
                              (unsigned long)fw_ver, (unsigned long)fw_build);
    }

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

    /* 时钟（标题栏副文本）：RTC 有效显示真实时间，否则降级 uptime——
     * 保证时钟始终在走（无 RTC 电池/未 SNTP 同步时界面不"死"） */
    if (s_data.rtc_valid) {
        lv_label_set_text_fmt(s_h_sub, "%02u:%02u:%02u",
                              (unsigned)s_data.rtc_h,
                              (unsigned)s_data.rtc_m,
                              (unsigned)s_data.rtc_s);
    } else {
        uint32_t up = s_data.uptime_s;
        lv_label_set_text_fmt(s_h_sub, "%02lu:%02lu:%02lu",
                              (unsigned long)(up / 3600u),
                              (unsigned long)((up % 3600u) / 60u),
                              (unsigned long)(up % 60u));
    }
}

/* 外设卡片分片：seg=0 → 卡片 0-2，seg=1 → 卡片 3-5 */
static void page_home_refresh_cards(uint8_t seg)
{
    if (seg == 0) {
        /* ETHERNET */
        if (s_data.eth_link) {
            card_set(0, GUI_STATE_OK, "%u.%u.%u.%u",
                     s_data.eth_ip[0], s_data.eth_ip[1],
                     s_data.eth_ip[2], s_data.eth_ip[3]);
        } else {
            card_set(0, GUI_STATE_ERR, "link down");
        }

        /* CAN */
        if (s_data.can_active) {
            card_set(1, (s_data.can_err == 0u) ? GUI_STATE_OK : GUI_STATE_WARN,
                     "tx%lu rx%lu", (unsigned long)s_data.can_tx,
                     (unsigned long)s_data.can_rx);
        } else {
            card_set(1, GUI_STATE_ERR, "offline");
        }

        /* IMU */
        if (s_data.imu_ready) {
            char r[16], p[16];
            ImuSvc_FormatFixed(s_data.imu_roll, 1, r);
            ImuSvc_FormatFixed(s_data.imu_pitch, 1, p);
            card_set(2, (s_data.imu_fault == 0u) ? GUI_STATE_OK : GUI_STATE_WARN,
                     "R%s P%s", r, p);
        } else {
            card_set(2, GUI_STATE_ERR, "offline");
        }
        return;
    }

    /* TOUCH */
    if (s_data.touch_state == TOUCH_EVT_DOWN ||
        s_data.touch_state == TOUCH_EVT_MOVE) {
        card_set(3, GUI_STATE_OK, "%u,%u",
                 (unsigned)s_data.touch_x, (unsigned)s_data.touch_y);
    } else {
        card_set(3, GUI_STATE_OK, "ready");
    }

    /* FLASH */
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
    /* 禁滚动：内容压缩在导航栏(288)之内，任何超屏内容宁可裁剪也不进入
     * 滚动模式——滚动拖动 = 大面积重绘 = 卡顿（实测来源，见 13.4） */
    lv_obj_clear_flag(s_scr_net, LV_OBJ_FLAG_SCROLLABLE);

    GuiTheme_TitleBar(s_scr_net, "Network", "monitor");

    /* 链路信息卡 */
    lv_obj_t *link = GuiTheme_Card(s_scr_net, 224, 54);
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
    lv_obj_t *ch = GuiTheme_Card(s_scr_net, 224, 80);
    lv_obj_set_pos(ch, 8, 92);
    lv_obj_t *ct = GuiTheme_Label(ch, "RATE  (pkts/s)", &lv_font_montserrat_12,
                                  GUI_COL_TEXT_DIM);
    lv_obj_set_pos(ct, 10, 4);

    s_n_chart = lv_chart_create(ch);
    lv_obj_set_size(s_n_chart, 208, 52);
    lv_obj_set_pos(s_n_chart, 8, 20);
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

    /* 统计网格 2×4（压缩：导航栏 288 以上，杜绝超屏滚动） */
    static const char *stat_names[8] = {
        "RX pkts", "TX pkts", "ICMP rx", "ICMP tx",
        "RTT min", "RTT avg", "RTT max", "ICMP pps",
    };
    for (uint32_t i = 0; i < 8u; i++) {
        lv_obj_t *c = GuiTheme_Card(s_scr_net, 108, 24);
        lv_obj_set_pos(c, (lv_coord_t)(8 + (i % 2u) * 116),
                       (lv_coord_t)(178 + (i / 2u) * 26));
        GuiTheme_Label(c, stat_names[i], &lv_font_montserrat_12,
                       GUI_COL_TEXT_DIM);
        lv_obj_align(lv_obj_get_child(c, lv_obj_get_child_cnt(c) - 1u),
                     LV_ALIGN_TOP_LEFT, 10, 1);
        s_n_stat[i] = GuiTheme_Label(c, "--", &lv_font_montserrat_14,
                                     GUI_COL_TEXT);
        lv_obj_align(s_n_stat[i], LV_ALIGN_TOP_RIGHT, -10, 1);
    }

    nav_build(s_scr_net);
}

/* 吞吐曲线追加（0.5s 节拍执行，与文本刷新错峰——避免每帧同时重绘
 * 大区域 chart 与多个 label 造成顿挫） */
static void page_net_curve(void)
{
    if (s_n_chart == NULL) {
        return;
    }
    /* SHIFT 模式：追加一点自动滚动 */
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

    /* 吞吐曲线由 0.5s 节拍 page_net_curve 追加（错峰，见上） */

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
    /* 禁滚动（见 13.4：滚动拖动 = 大面积重绘卡顿） */
    lv_obj_clear_flag(s_scr_sys, LV_OBJ_FLAG_SCROLLABLE);

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
    lv_obj_set_style_anim_time(s_s_heap_bar, 250, 0);  /* 堆条平滑过渡时长 */
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

/* ---------------- CPU 环平滑动画：值从当前渐变到新值（200ms ease-out） ----------------
 * 直接 lv_arc_set_value 每秒跳变（视觉顿挫）；动画过渡后环形表丝滑跟随 */
static void cpu_arc_anim_cb(void *var, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)var, (lv_coord_t)v);
}

static void page_sys_set_cpu(uint32_t pct)
{
    if (s_s_cpu_arc == NULL || s_s_cpu_pct == NULL) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_s_cpu_arc);
    lv_anim_set_exec_cb(&a, cpu_arc_anim_cb);
    lv_anim_set_values(&a, (int32_t)lv_arc_get_value(s_s_cpu_arc),
                       (int32_t)pct);
    lv_anim_set_time(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_label_set_text_fmt(s_s_cpu_pct, "%lu%%", (unsigned long)pct);
}

static void page_sys_refresh(void)
{
    uint32_t ver = *(volatile uint32_t *)OTA_APP_VERSION_ADDR;
    lv_label_set_text_fmt(s_s_fw, "Firmware v%lu  Plan-B",
                          (unsigned long)ver);
    lv_label_set_text_fmt(s_s_crash, "Crash #%lu   Tasks %lu",
                          (unsigned long)s_data.crash_seq,
                          (unsigned long)s_data.task_count);

    /* CPU 环形表（动画平滑过渡） */
    page_sys_set_cpu(s_data.cpu_percent);

    /* 堆（bar 内置动画：平滑滑动到新值） */
    uint32_t used = (s_data.heap_total > s_data.heap_free)
                        ? (s_data.heap_total - s_data.heap_free) : 0u;
    uint32_t pct = (s_data.heap_total > 0u)
                       ? (used * 100u / s_data.heap_total) : 0u;
    lv_bar_set_value(s_s_heap_bar, (lv_coord_t)pct, LV_ANIM_ON);
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
 * CAM 页：摄像头链路（UART5）实时状态——帧统计 / 手部 / 手势 / 挥手
 * 数据源：cam_link 服务层（ISR 解析帧协议，本页 250ms 刷新读取）
 * ================================================================ */
static void page_cam_build(void)
{
    s_scr_cam = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_cam, GUI_COL_BG, 0);
    lv_obj_set_style_bg_opa(s_scr_cam, LV_OPA_COVER, 0);

    /* 页眉（与其它页一致：标题 + 副文本） */
    GuiTheme_Label(s_scr_cam, LV_SYMBOL_IMAGE " CAM",
                   &lv_font_montserrat_16, GUI_COL_PRIMARY);
    lv_obj_align(lv_obj_get_child(s_scr_cam, lv_obj_get_child_cnt(s_scr_cam) - 1u),
                 LV_ALIGN_TOP_LEFT, 10, 6);
    GuiTheme_Label(s_scr_cam, "OpenART UART5 link",
                   &lv_font_montserrat_12, GUI_COL_ACCENT);
    lv_obj_align(lv_obj_get_child(s_scr_cam, lv_obj_get_child_cnt(s_scr_cam) - 1u),
                 LV_ALIGN_TOP_RIGHT, -10, 6);

    /* 链路统计卡 */
    lv_obj_t *st = GuiTheme_Card(s_scr_cam, 224, 76);
    lv_obj_set_pos(st, 8, 40);
    GuiTheme_Label(st, "LINK", &lv_font_montserrat_12, GUI_COL_TEXT_DIM);
    lv_obj_align(lv_obj_get_child(st, lv_obj_get_child_cnt(st) - 1u),
                 LV_ALIGN_TOP_LEFT, 10, 6);
    s_c_dot = lv_obj_create(st);
    lv_obj_remove_style_all(s_c_dot);
    lv_obj_set_size(s_c_dot, 10, 10);
    lv_obj_align(s_c_dot, LV_ALIGN_TOP_LEFT, 56, 10);
    lv_obj_set_style_bg_color(s_c_dot, GUI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_c_dot, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_c_dot, LV_OPA_COVER, LV_PART_MAIN);
    s_c_frames = GuiTheme_Label(st, "frames --", &lv_font_montserrat_12,
                                GUI_COL_TEXT);
    lv_obj_set_pos(s_c_frames, 10, 28);
    s_c_err = GuiTheme_Label(st, "err --", &lv_font_montserrat_12,
                             GUI_COL_TEXT);
    lv_obj_set_pos(s_c_err, 118, 28);
    s_c_swipe = GuiTheme_Label(st, "swipe --", &lv_font_montserrat_12,
                               GUI_COL_TEXT);
    lv_obj_set_pos(s_c_swipe, 10, 52);
    s_c_last = GuiTheme_Label(st, "last --ms", &lv_font_montserrat_12,
                              GUI_COL_TEXT);
    lv_obj_set_pos(s_c_last, 118, 52);

    /* 手部卡 */
    lv_obj_t *hd = GuiTheme_Card(s_scr_cam, 224, 76);
    lv_obj_set_pos(hd, 8, 124);
    GuiTheme_Label(hd, "HAND", &lv_font_montserrat_12, GUI_COL_TEXT_DIM);
    lv_obj_align(lv_obj_get_child(hd, lv_obj_get_child_cnt(hd) - 1u),
                 LV_ALIGN_TOP_LEFT, 10, 6);
    s_c_hand = GuiTheme_Label(hd, "state --", &lv_font_montserrat_12,
                              GUI_COL_TEXT);
    lv_obj_set_pos(s_c_hand, 10, 28);
    s_c_pos = GuiTheme_Label(hd, "pos --", &lv_font_montserrat_12,
                             GUI_COL_TEXT);
    lv_obj_set_pos(s_c_pos, 118, 28);
    s_c_size = GuiTheme_Label(hd, "size --", &lv_font_montserrat_12,
                              GUI_COL_TEXT);
    lv_obj_set_pos(s_c_pos, 10, 52);
    s_c_size = GuiTheme_Label(hd, "size --", &lv_font_montserrat_12,
                              GUI_COL_TEXT);
    lv_obj_set_pos(s_c_size, 118, 52);

    /* 手势卡 */
    lv_obj_t *gs = GuiTheme_Card(s_scr_cam, 224, 76);
    lv_obj_set_pos(gs, 8, 208);
    GuiTheme_Label(gs, "GESTURE", &lv_font_montserrat_12, GUI_COL_TEXT_DIM);
    lv_obj_align(lv_obj_get_child(gs, lv_obj_get_child_cnt(gs) - 1u),
                 LV_ALIGN_TOP_LEFT, 10, 6);
    s_c_gesture = GuiTheme_Label(gs, "--", &lv_font_montserrat_16,
                                 GUI_COL_PRIMARY);
    lv_obj_set_pos(s_c_gesture, 10, 26);
    s_c_conf = GuiTheme_Label(gs, "conf --%", &lv_font_montserrat_12,
                              GUI_COL_TEXT);
    lv_obj_set_pos(s_c_conf, 120, 30);
    s_c_swl = GuiTheme_Label(gs, "L:-", &lv_font_montserrat_12,
                             GUI_COL_WARN);
    lv_obj_set_pos(s_c_swl, 10, 54);
    s_c_swr = GuiTheme_Label(gs, "R:-", &lv_font_montserrat_12,
                             GUI_COL_WARN);
    lv_obj_set_pos(s_c_swr, 60, 54);

    nav_build(s_scr_cam);
}

/* 250ms 刷新（仅 CAM 页可见时由 RefreshFast 调用） */
static void page_cam_refresh(void)
{
    cam_link_state_t cs;
    const volatile cam_link_state_t *p = CamLink_GetState();
    memcpy(&cs, (const void *)p, sizeof(cs));

    /* 链路状态：有帧且 <1s 前 → 绿，否则灰 */
    uint32_t now = BSP_GetTick();
    uint8_t alive = (cs.frame_count > 0u) &&
                    ((now - cs.last_rx_ms) < 1000u);
    lv_obj_set_style_bg_color(s_c_dot, alive ? GUI_COL_OK : GUI_COL_BORDER, 0);

    lv_label_set_text_fmt(s_c_frames, "frames %lu", (unsigned long)cs.frame_count);
    lv_label_set_text_fmt(s_c_err, "err %lu", (unsigned long)cs.err_count);
    lv_label_set_text_fmt(s_c_swipe, "swipe %lu", (unsigned long)cs.swipe_count);
    lv_label_set_text_fmt(s_c_last, "last %lums",
                          (unsigned long)((now >= cs.last_rx_ms) ? (now - cs.last_rx_ms) : 0u));

    lv_label_set_text(s_c_hand, cs.hand_present ? "state ON " : "state OFF");
    lv_label_set_text_fmt(s_c_pos, "pos %u,%u",
                          (unsigned)cs.hand_x, (unsigned)cs.hand_y);
    lv_label_set_text_fmt(s_c_size, "size %ux%u",
                          (unsigned)cs.hand_w, (unsigned)cs.hand_h);

    lv_label_set_text(s_c_gesture, CamLink_GestureName(cs.gesture_id));
    lv_label_set_text_fmt(s_c_conf, "conf %u%%", (unsigned)cs.gesture_conf);
    lv_label_set_text_fmt(s_c_swl, "L:%s", cs.swipe_left ? "!" : "-");
    lv_label_set_text_fmt(s_c_swr, "R:%s", cs.swipe_right ? "!" : "-");
}

/* ================================================================
 * GIMBAL 云台模型页（虚拟云台视觉演示）
 * 布局：视场模拟（目标+准星）→ PAN/TILT 弧形仪表 → 状态行
 * 刷新：独立 50ms 定时器（20fps），仅页面可见时执行
 * ================================================================ */
static void gimbal_anim_cb(lv_timer_t *tmr);   /* 前向声明 */
static void gimbal_ch_click(lv_event_t *e);    /* 通道切换按钮回调 */
static void gimbal_pid_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                           const char *txt, uint8_t tag,
                           lv_coord_t w, lv_coord_t h);  /* 参数加减按钮 */
static void gimbal_pid_btn_click(lv_event_t *e);
#define G_FOV_X     16      /* 视场内区原点（页面坐标） */
#define G_FOV_Y     60
#define G_FOV_W     208
#define G_FOV_H     158
#define G_TGT_HALF  11      /* 目标块半宽（22px） */
#define G_PAN_MAX   90.0f
#define G_TILT_MAX  45.0f

/* 视场网格（静态：4 竖 3 横细线；lv_line 引用点数组，每条线必须独立数组；
 * scene 为卡内子对象，坐标从 0 起） */
static void gimbal_fov_grid(lv_obj_t *parent)
{
    lv_color_t c = lv_color_hex(0x1A2434);
    static lv_point_t vps[4][2];
    static lv_point_t hps[3][2];
    for (int i = 1; i < 4; i++) {
        vps[i][0].x = G_FOV_W * i / 4;
        vps[i][0].y = 0;
        vps[i][1].x = vps[i][0].x;
        vps[i][1].y = G_FOV_H;
        lv_obj_t *l = lv_line_create(parent);
        lv_line_set_points(l, vps[i], 2);
        lv_obj_set_style_line_color(l, c, 0);
        lv_obj_set_style_line_width(l, 1, 0);
        lv_obj_set_style_line_opa(l, LV_OPA_40, 0);
    }
    for (int i = 1; i < 3; i++) {
        hps[i][0].x = 0;
        hps[i][0].y = G_FOV_H * i / 3;
        hps[i][1].x = G_FOV_W;
        hps[i][1].y = hps[i][0].y;
        lv_obj_t *l = lv_line_create(parent);
        lv_line_set_points(l, hps[i], 2);
        lv_obj_set_style_line_color(l, c, 0);
        lv_obj_set_style_line_width(l, 1, 0);
        lv_obj_set_style_line_opa(l, LV_OPA_40, 0);
    }
}

/* 弧形仪表（简洁清晰）：半圆弧 + 细刻度 + 长刻度（0° 位）+ 指针 + 中心点。
 * 不显示刻度数字（lv_meter 无法自定义标签文本，0/90/180 不直观）——
 * 数值由卡内读数标签（ACCENT 色）传达，量程由卡内 ± 标签传达。 */
static lv_obj_t *gimbal_meter(lv_obj_t *parent, lv_meter_indicator_t **ind_out)
{
    lv_obj_t *m = lv_meter_create(parent);
    lv_obj_set_size(m, 100, 48);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_meter_scale_t *sc = lv_meter_add_scale(m);
    lv_meter_set_scale_range(m, sc, 0, 180, 270, 90);
    lv_meter_set_scale_ticks(m, sc, 9, 2, 6, lv_color_hex(0x3A4658));
    lv_meter_set_scale_major_ticks(m, sc, 3, 3, 12, lv_color_hex(0x8A97A8), 0);
    *ind_out = lv_meter_add_needle_line(m, sc, 2, GUI_COL_ACCENT, 22);
    return m;
}

static void page_gimbal_build(void)
{
    s_scr_gimbal = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_gimbal, GUI_COL_BG, 0);
    lv_obj_set_style_bg_opa(s_scr_gimbal, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr_gimbal, LV_OBJ_FLAG_SCROLLABLE);

    GuiTheme_Label(s_scr_gimbal, LV_SYMBOL_IMAGE " GIMBAL",
                   &lv_font_montserrat_16, GUI_COL_PRIMARY);
    lv_obj_align(lv_obj_get_child(s_scr_gimbal, lv_obj_get_child_cnt(s_scr_gimbal) - 1u),
                 LV_ALIGN_TOP_LEFT, 10, 6);
    /* 标题栏副文本 = 实时性能显示（DEMO 标注 + fps + 渲染耗时） */
    s_g_perf = GuiTheme_Label(s_scr_gimbal, "DEMO --fps --ms",
                               &lv_font_montserrat_12, GUI_COL_ACCENT);
    lv_obj_align(s_g_perf, LV_ALIGN_TOP_RIGHT, -10, 6);

    /* ---- 视场卡：模拟摄像头画面（HUD 风格：顶条状态 + 大视场） ---- */
    lv_obj_t *fov = GuiTheme_Card(s_scr_gimbal, 220, 188);
    lv_obj_set_pos(fov, 10, 34);
    /* HUD 顶条：状态 + 偏差读数 + 通道切换按钮（相机取景风格） */
    lv_obj_t *hud = lv_obj_create(fov);
    lv_obj_remove_style_all(hud);
    lv_obj_set_pos(hud, 6, 6);
    lv_obj_set_size(hud, 208, 18);
    lv_obj_set_style_border_color(hud, lv_color_hex(0x2A3548), 0);
    lv_obj_set_style_border_width(hud, 1, 0);
    lv_obj_set_style_border_side(hud, LV_BORDER_SIDE_BOTTOM, 0);
    s_g_track_dot = GuiTheme_Dot(hud);
    lv_obj_set_pos(s_g_track_dot, 0, 5);
    lv_obj_t *tl = GuiTheme_Label(hud, "TRACK", &lv_font_montserrat_12,
                                  GUI_COL_TEXT_DIM);
    lv_obj_set_pos(tl, 12, 2);
    s_g_dx = GuiTheme_Label(hud, "dx +000", &lv_font_montserrat_12, GUI_COL_TEXT);
    lv_obj_set_pos(s_g_dx, 84, 2);
    s_g_dy = GuiTheme_Label(hud, "dy +000", &lv_font_montserrat_12, GUI_COL_TEXT);
    lv_obj_set_pos(s_g_dy, 150, 2);
    /* 通道切换按钮（循环：DEMO→RAW→COMP→KF→DEMO） */
    s_g_ch_btn = lv_btn_create(hud);
    lv_obj_remove_style_all(s_g_ch_btn);
    lv_obj_set_size(s_g_ch_btn, 44, 16);
    lv_obj_set_pos(s_g_ch_btn, 162, 0);
    lv_obj_set_style_radius(s_g_ch_btn, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_g_ch_btn, lv_color_hex(0x1F2B3D), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_g_ch_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_g_ch_btn, GUI_COL_CARD_HI, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_g_ch_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_g_ch_btn, GUI_COL_BORDER, LV_PART_MAIN);
    lv_obj_t *ch_lab = lv_label_create(s_g_ch_btn);
    lv_label_set_text(ch_lab, "DEMO");
    lv_obj_set_style_text_font(ch_lab, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(ch_lab, GUI_COL_ACCENT, LV_PART_MAIN);
    lv_obj_center(ch_lab);
    lv_obj_add_event_cb(s_g_ch_btn, gimbal_ch_click, LV_EVENT_CLICKED, NULL);

    /* 视场底（更深的"取景"感） */
    lv_obj_t *scene = lv_obj_create(fov);
    lv_obj_remove_style_all(scene);
    lv_obj_set_pos(scene, 6, 26);
    lv_obj_set_size(scene, G_FOV_W, G_FOV_H);
    lv_obj_set_style_bg_color(scene, lv_color_hex(0x0A0E16), 0);
    lv_obj_set_style_bg_opa(scene, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(scene, lv_color_hex(0x2A3548), 0);
    lv_obj_set_style_border_width(scene, 1, 0);
    gimbal_fov_grid(scene);

    /* 目标块（apriltag 风格：黑底白边 + 白心），初始居中偏右 */
    s_g_target = lv_obj_create(scene);
    lv_obj_remove_style_all(s_g_target);
    lv_obj_set_size(s_g_target, G_TGT_HALF * 2, G_TGT_HALF * 2);
    lv_obj_set_style_bg_color(s_g_target, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_g_target, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_g_target, lv_color_hex(0xECEFF1), 0);
    lv_obj_set_style_border_width(s_g_target, 2, 0);
    /* 阴影每帧重绘开销大（动画流畅度优先），取消 */
    s_g_core = lv_obj_create(s_g_target);
    lv_obj_remove_style_all(s_g_core);
    lv_obj_set_size(s_g_core, 8, 8);
    lv_obj_align(s_g_core, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_g_core, lv_color_hex(0xECEFF1), 0);
    lv_obj_set_style_bg_opa(s_g_core, LV_OPA_COVER, 0);

    /* 准星：单 lv_canvas（30×30 透明画布，十字+圆环+中心点一次绘制；
     * 1 个对象平移 = 2 个失效区域，远小于 4 对象方案；缓冲受 SRAM 预算限制） */
    s_g_cross_canvas = lv_canvas_create(scene);
    lv_canvas_set_buffer(s_g_cross_canvas, s_g_cross_cbuf, 30, 30,
                         LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_obj_set_style_radius(s_g_cross_canvas, 0, 0);
    lv_obj_set_style_bg_opa(s_g_cross_canvas, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(s_g_cross_canvas, 0, 0);

    /* ---- PAN/TILT 双仪表卡（含义符号 + 读数 + 量程标签） ---- */
    s_g_pan_card = GuiTheme_Card(s_scr_gimbal, 106, 64);
    lv_obj_set_pos(s_g_pan_card, 10, 224);
    lv_obj_t *pan_lab = GuiTheme_Label(s_g_pan_card, LV_SYMBOL_LEFT " PAN",
                                       &lv_font_montserrat_12, GUI_COL_TEXT_DIM);
    lv_obj_set_pos(pan_lab, 6, 2);
    s_g_pan_val = GuiTheme_Label(s_g_pan_card, "+0.0°", &lv_font_montserrat_14,
                                 GUI_COL_ACCENT);
    lv_obj_set_pos(s_g_pan_val, 56, 2);
    s_g_pan_meter = gimbal_meter(s_g_pan_card, &s_g_pan_ind);
    lv_obj_set_pos(s_g_pan_meter, 3, 14);
    GuiTheme_Label(s_g_pan_card, "-90°", &lv_font_montserrat_10, GUI_COL_TEXT_DIM);
    lv_obj_set_pos(lv_obj_get_child(s_g_pan_card, lv_obj_get_child_cnt(s_g_pan_card) - 1u), 2, 52);
    GuiTheme_Label(s_g_pan_card, "+90°", &lv_font_montserrat_10, GUI_COL_TEXT_DIM);
    lv_obj_set_pos(lv_obj_get_child(s_g_pan_card, lv_obj_get_child_cnt(s_g_pan_card) - 1u), 80, 52);

    s_g_tilt_card = GuiTheme_Card(s_scr_gimbal, 106, 64);
    lv_obj_set_pos(s_g_tilt_card, 124, 224);
    lv_obj_t *tilt_lab = GuiTheme_Label(s_g_tilt_card, LV_SYMBOL_UP " TILT",
                                        &lv_font_montserrat_12, GUI_COL_TEXT_DIM);
    lv_obj_set_pos(tilt_lab, 6, 2);
    s_g_tilt_val = GuiTheme_Label(s_g_tilt_card, "+0.0°", &lv_font_montserrat_14,
                                  GUI_COL_ACCENT);
    lv_obj_set_pos(s_g_tilt_val, 56, 2);
    s_g_tilt_meter = gimbal_meter(s_g_tilt_card, &s_g_tilt_ind);
    lv_obj_set_pos(s_g_tilt_meter, 3, 14);
    GuiTheme_Label(s_g_tilt_card, "-45°", &lv_font_montserrat_10, GUI_COL_TEXT_DIM);
    lv_obj_set_pos(lv_obj_get_child(s_g_tilt_card, lv_obj_get_child_cnt(s_g_tilt_card) - 1u), 2, 52);
    GuiTheme_Label(s_g_tilt_card, "+45°", &lv_font_montserrat_10, GUI_COL_TEXT_DIM);
    lv_obj_set_pos(lv_obj_get_child(s_g_tilt_card, lv_obj_get_child_cnt(s_g_tilt_card) - 1u), 78, 52);

    /* ---- PID 调参实验室控件（初始隐藏，PID 模式显示） ---- */
    /* 三条曲线线对象挂在 GIMBAL 屏上（坐标=视场区，覆盖 scene 上方） */
    s_g_curve_set = lv_line_create(s_scr_gimbal);
    lv_obj_set_style_line_color(s_g_curve_set, GUI_COL_ACCENT, 0);
    lv_obj_set_style_line_width(s_g_curve_set, 2, 0);
    lv_obj_set_style_line_opa(s_g_curve_set, LV_OPA_80, 0);
    s_g_curve_y = lv_line_create(s_scr_gimbal);
    lv_obj_set_style_line_color(s_g_curve_y, GUI_COL_OK, 0);
    lv_obj_set_style_line_width(s_g_curve_y, 2, 0);
    s_g_curve_u = lv_line_create(s_scr_gimbal);
    lv_obj_set_style_line_color(s_g_curve_u, GUI_COL_WARN, 0);
    lv_obj_set_style_line_width(s_g_curve_u, 1, 0);
    lv_obj_set_style_line_opa(s_g_curve_u, LV_OPA_70, 0);
    lv_obj_add_flag(s_g_curve_set, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_g_curve_y, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_g_curve_u, LV_OBJ_FLAG_HIDDEN);

    /* Kp/Kd 大卡 + Ki 大卡（106 宽，16px 大字号，PID 模式替换仪表卡） */
    s_g_kp_card = GuiTheme_Card(s_scr_gimbal, 106, 64);
    lv_obj_set_pos(s_g_kp_card, 10, 224);
    s_g_ki_card = GuiTheme_Card(s_scr_gimbal, 106, 64);
    lv_obj_set_pos(s_g_ki_card, 124, 224);
    s_g_kd_card = NULL;   /* Kd 并入 Kp 卡第二行 */
    /* Kp 卡：标题 + 大值 + 主按钮 + Kd 行 */
    GuiTheme_Label(s_g_kp_card, "Kp", &lv_font_montserrat_12, GUI_COL_TEXT_DIM);
    lv_obj_set_pos(lv_obj_get_child(s_g_kp_card, lv_obj_get_child_cnt(s_g_kp_card) - 1u), 6, 3);
    s_g_kp_val = GuiTheme_Label(s_g_kp_card, "1.0", &lv_font_montserrat_16,
                                GUI_COL_ACCENT);
    lv_obj_set_pos(s_g_kp_val, 38, 1);
    gimbal_pid_btn(s_g_kp_card, 6, 20, "-", 0, 40, 22);   /* Kp- */
    gimbal_pid_btn(s_g_kp_card, 54, 20, "+", 1, 40, 22);  /* Kp+ */
    GuiTheme_Label(s_g_kp_card, "Kd", &lv_font_montserrat_10, GUI_COL_TEXT_DIM);
    lv_obj_set_pos(lv_obj_get_child(s_g_kp_card, lv_obj_get_child_cnt(s_g_kp_card) - 1u), 6, 45);
    s_g_kd_val = GuiTheme_Label(s_g_kp_card, "0.00", &lv_font_montserrat_12,
                                GUI_COL_ACCENT);
    lv_obj_set_pos(s_g_kd_val, 34, 45);
    gimbal_pid_btn(s_g_kp_card, 62, 44, "-", 4, 20, 16);  /* Kd- */
    gimbal_pid_btn(s_g_kp_card, 84, 44, "+", 5, 20, 16);  /* Kd+ */
    /* Ki 卡：标题 + 大值 + 主按钮 */
    GuiTheme_Label(s_g_ki_card, "Ki", &lv_font_montserrat_12, GUI_COL_TEXT_DIM);
    lv_obj_set_pos(lv_obj_get_child(s_g_ki_card, lv_obj_get_child_cnt(s_g_ki_card) - 1u), 6, 3);
    s_g_ki_val = GuiTheme_Label(s_g_ki_card, "0.0", &lv_font_montserrat_16,
                                GUI_COL_ACCENT);
    lv_obj_set_pos(s_g_ki_val, 38, 1);
    gimbal_pid_btn(s_g_ki_card, 6, 20, "-", 2, 40, 22);   /* Ki- */
    gimbal_pid_btn(s_g_ki_card, 54, 20, "+", 3, 40, 22);  /* Ki+ */
    lv_obj_add_flag(s_g_kp_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_g_ki_card, LV_OBJ_FLAG_HIDDEN);

    /* PID 初始化：虚拟二阶对象 + 全特性 PID */
    s_pid_lab.kp = 1.0f;
    s_pid_lab.ki = 0.0f;
    s_pid_lab.kd = 0.0f;
    s_pid_lab.dt = 0.033f;
    s_pid_lab.out_min = -40.0f;
    s_pid_lab.out_max = 40.0f;
    PID_Pos_Init(&s_pid_lab);
    s_plant_y = 0.0f;
    s_plant_ydot = 0.0f;
    s_plant_u_d[0] = s_plant_u_d[1] = s_plant_u_d[2] = 0.0f;
    s_pid_run_mode = 0;
    s_pid_t = 0.0f;
    s_cv_n = 0;
    s_cv_tick = 0;

    s_g_demo_t = 0.0f;
    s_g_follow_x = G_FOV_W * 0.7f;
    s_g_follow_y = G_FOV_H * 0.5f;
    lv_obj_set_pos(s_g_target, (lv_coord_t)(s_g_follow_x - G_TGT_HALF),
                   (lv_coord_t)(s_g_follow_y - G_TGT_HALF));
    GuiTheme_DotSet(s_g_track_dot, GUI_STATE_OK);
    nav_build(s_scr_gimbal);

    /* 独立动画定时器（33ms ≈ 30fps，刷新调度 LV_DISP_DEF_REFR_PERIOD=16ms
     * 已是最快档；回调 gimbal_anim_cb 定义于页面切换段（需 s_active）） */
    s_g_anim_timer = lv_timer_create(gimbal_anim_cb, 33, NULL);
    (void)s_g_anim_timer;   /* 句柄保留：后续页面隐藏时 pause/resume 用 */
    s_g_frame_cnt = 0;
    s_g_last_ms = lv_tick_get();
    s_g_mode = 0;           /* 默认 DEMO 演示 */
}

/* PID 参数加减按钮：tag 0..5 = Kp-/Kp+/Ki-/Ki+/Kd-/Kd+；w/h 可指定尺寸 */
static void gimbal_pid_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                           const char *txt, uint8_t tag,
                           lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1F2B3D), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, GUI_COL_CARD_HI, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, GUI_COL_BORDER, LV_PART_MAIN);
    lv_obj_t *lab = lv_label_create(b);
    lv_label_set_text(lab, txt);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lab, GUI_COL_TEXT, LV_PART_MAIN);
    lv_obj_center(lab);
    lv_obj_add_event_cb(b, gimbal_pid_btn_click, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)tag);
}

static void gimbal_pid_btn_click(lv_event_t *e)
{
    uint8_t tag = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    switch (tag) {
    case 0: s_pid_lab.kp = (s_pid_lab.kp > 0.05f) ? (s_pid_lab.kp - 0.5f) : 0.0f; break;
    case 1: s_pid_lab.kp += 0.5f; break;
    case 2: s_pid_lab.ki = (s_pid_lab.ki > 0.05f) ? (s_pid_lab.ki - 0.1f) : 0.0f; break;
    case 3: s_pid_lab.ki += 0.1f; break;
    case 4: s_pid_lab.kd = (s_pid_lab.kd > 0.005f) ? (s_pid_lab.kd - 0.02f) : 0.0f; break;
    default: s_pid_lab.kd += 0.02f; break;
    }
    /* 曲线清空重来（新参数下观察全新响应） */
    s_cv_n = 0;
    s_pid_t = 0.0f;
    s_plant_y = 0.0f;
    s_plant_ydot = 0.0f;
}

/* PID 实验室单步：设定值 → ctrl PID → 虚拟二阶对象 → 曲线采样 */
static void gimbal_pid_step(void)
{
    s_pid_t += 0.033f;
    /* 设定值（模式：0=阶跃 1=正弦 2=定值+真实扰动）；幅度限制在 ±40 量程内 */
    float set = 0.0f;
    if (s_pid_run_mode == 0u) {
        set = (s_pid_t > 0.2f) ? 30.0f : 0.0f;
    } else if (s_pid_run_mode == 1u) {
        set = 25.0f + 12.0f * sinf(s_pid_t * 3.1416f);   /* 13..37，不越界 */
    } else {
        set = 15.0f;
    }
    /* 测量 = 对象输出 + 真实扰动（MPU6050 roll，DIST 模式） */
    float dist = (s_pid_run_mode == 2u) ? 0.3f * ImuSvc_GetState()->roll : 0.0f;
    float y_meas = s_plant_y + dist;

    /* ctrl 库 PID（全特性：积分分离/微分低通/限幅） */
    float u = PID_Pos_Update(&s_pid_lab, set - y_meas);

    /* 虚拟二阶对象（欠阻尼 ζ=0.3，振荡可见；2 步输出延迟） */
    float u_d = s_plant_u_d[2];
    s_plant_u_d[2] = s_plant_u_d[1];
    s_plant_u_d[1] = s_plant_u_d[0];
    s_plant_u_d[0] = u;
    const float wn = 12.0f, zeta = 0.3f;
    float ddot = wn * wn * (u_d - s_plant_y) - 2.0f * zeta * wn * s_plant_ydot;
    s_plant_ydot += ddot * 0.033f;
    s_plant_y += s_plant_ydot * 0.033f;

    /* 曲线采样（每 3 帧一点 ≈100ms，40 点 = 4s 窗口；降频保流畅） */
    if (++s_cv_tick >= 3u) {
        s_cv_tick = 0;
        if (s_cv_n < PID_CV_PTS) s_cv_n++;
        for (uint32_t i = s_cv_n; i > 0; i--) {
            s_cv_set[i] = s_cv_set[i - 1];
            s_cv_y[i]   = s_cv_y[i - 1];
            s_cv_u[i]   = s_cv_u[i - 1];
        }
        s_cv_set[0] = set;
        s_cv_y[0]   = y_meas;
        s_cv_u[0]   = u;
    }
    /* 曲线点映射：±40 → 视场高度（clamp 防越界） */
    for (uint32_t i = 0; i < s_cv_n; i++) {
        s_cv_pts_set[i].x = (lv_coord_t)(G_FOV_X + (lv_coord_t)i * (G_FOV_W - 1)
                                         / (PID_CV_PTS - 1));
        float sv = s_cv_set[i];
        if (sv > 40.0f) sv = 40.0f;
        else if (sv < -40.0f) sv = -40.0f;
        s_cv_pts_set[i].y = (lv_coord_t)(G_FOV_Y + (40.0f - sv) / 80.0f * G_FOV_H);
        s_cv_pts_y[i].x = s_cv_pts_set[i].x;
        float yv = s_cv_y[i];
        if (yv > 40.0f) yv = 40.0f;
        else if (yv < -40.0f) yv = -40.0f;
        s_cv_pts_y[i].y = (lv_coord_t)(G_FOV_Y + (40.0f - yv) / 80.0f * G_FOV_H);
        s_cv_pts_u[i].x = s_cv_pts_set[i].x;
        float uv = s_cv_u[i];
        if (uv > 40.0f) uv = 40.0f;
        else if (uv < -40.0f) uv = -40.0f;
        s_cv_pts_u[i].y = (lv_coord_t)(G_FOV_Y + (40.0f - uv) / 80.0f * G_FOV_H);
    }
    if (s_cv_n >= 2u) {
        lv_line_set_points(s_g_curve_set, s_cv_pts_set, s_cv_n);
        lv_line_set_points(s_g_curve_y, s_cv_pts_y, s_cv_n);
        lv_line_set_points(s_g_curve_u, s_cv_pts_u, s_cv_n);
    }

    /* 参数卡数值 */
    lv_label_set_text_fmt(s_g_kp_val, "%.1f", (double)s_pid_lab.kp);
    lv_label_set_text_fmt(s_g_ki_val, "%.1f", (double)s_pid_lab.ki);
    lv_label_set_text_fmt(s_g_kd_val, "%.2f", (double)s_pid_lab.kd);

    /* HUD：模式名 + 误差/输出 */
    static const char *const run_name[3] = { "STEP", "SINE", "DIST" };
    lv_label_set_text_fmt(s_g_dx, "e%+.1f", (double)(set - y_meas));
    lv_label_set_text_fmt(s_g_dy, "u%+.1f", (double)u);
    GuiTheme_DotSet(s_g_track_dot, GUI_STATE_OK);

    /* 标题栏：模式 + fps */
    uint32_t now = lv_tick_get();
    uint32_t dt = now - s_g_last_ms;
    s_g_last_ms = now;
    if (dt > 0u) {
        lv_label_set_text_fmt(s_g_perf, "%s %ufps %.1fms",
                              run_name[s_pid_run_mode], (unsigned)(1000u / dt),
                              (double)g_gui_render_us / 1000.0);
    }
}

/* 动画刷新（33ms 定时器驱动 ≈30fps；仅 GIMBAL 页可见时执行）
 * 通道模式：DEMO（Lissajous 演示）/ RAW（加速度原始）/ COMP（互补）/ KF（卡尔曼） */
static void page_gimbal_refresh(void)
{
    float tx, ty;
    float pan, tilt;

    /* PID 实验室：独立逻辑（曲线 + 参数卡） */
    if (s_g_mode == 4u) {
        gimbal_pid_step();
        return;
    }

    if (s_g_mode == 0u) {
        /* ---- DEMO：Lissajous 演示轨迹 ---- */
        s_g_demo_t += 0.033f;
        float t = s_g_demo_t;
        tx = G_FOV_W * 0.5f + G_FOV_W * 0.38f * sinf(t * 0.5236f);
        ty = G_FOV_H * 0.5f + G_FOV_H * 0.34f * sinf(t * 0.3491f);
        s_g_follow_x += (tx - s_g_follow_x) * 0.40f;
        s_g_follow_y += (ty - s_g_follow_y) * 0.40f;
        pan  = (s_g_follow_x - G_FOV_W * 0.5f) / (G_FOV_W * 0.5f) * G_PAN_MAX;
        tilt = -(s_g_follow_y - G_FOV_H * 0.5f) / (G_FOV_H * 0.5f) * G_TILT_MAX;
        lv_obj_clear_flag(s_g_target, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_g_target, (lv_coord_t)(tx - G_TGT_HALF),
                       (lv_coord_t)(ty - G_TGT_HALF));
    } else {
        /* ---- REAL：MPU6050 真实姿态（滤波对比通道） ---- */
        const imu_svc_state_t *imu = ImuSvc_GetState();
        float r, p;
        if (s_g_mode == 1u) { r = imu->acc_roll;  p = imu->acc_pitch; }
        else if (s_g_mode == 2u) { r = imu->comp_roll; p = imu->comp_pitch; }
        else { r = imu->roll; p = imu->pitch; }
        pan  = r;
        tilt = p;
        /* 姿态 → 视场映射：roll ±90 → 水平 ±42%，pitch ±45 → 垂直 ±40% */
        tx = G_FOV_W * 0.5f + (r / G_PAN_MAX) * G_FOV_W * 0.42f;
        ty = G_FOV_H * 0.5f - (p / G_TILT_MAX) * G_FOV_H * 0.40f;
        s_g_follow_x += (tx - s_g_follow_x) * 0.30f;
        s_g_follow_y += (ty - s_g_follow_y) * 0.30f;
        /* REAL 模式：目标块隐藏（准星 = 板子姿态，中心 = 水平基准） */
        lv_obj_add_flag(s_g_target, LV_OBJ_FLAG_HIDDEN);
    }

    /* 准星画布：每帧重绘（透明底 + 十字圆头线 + 圆环 + 中心点）+ 单对象平移 */
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = GUI_COL_ACCENT;
    ld.width = 2;
    ld.round_start = 1;
    ld.round_end = 1;
    lv_point_t hp[2] = {{3, 15}, {27, 15}};
    lv_point_t vp[2] = {{15, 3}, {15, 27}};
    lv_canvas_fill_bg(s_g_cross_canvas, GUI_COL_BG, LV_OPA_TRANSP);
    lv_canvas_draw_line(s_g_cross_canvas, hp, 2, &ld);
    lv_canvas_draw_line(s_g_cross_canvas, vp, 2, &ld);
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = GUI_COL_ACCENT;
    ad.width = 2;
    lv_canvas_draw_arc(s_g_cross_canvas, 15, 15, 7, 0, 360, &ad);
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = GUI_COL_ACCENT;
    rd.radius = 2;
    lv_canvas_draw_rect(s_g_cross_canvas, 13, 13, 5, 5, &rd);
    lv_obj_set_pos(s_g_cross_canvas, (lv_coord_t)(s_g_follow_x - 15),
                   (lv_coord_t)(s_g_follow_y - 15));

    /* 仪表联动（每 6 帧更新——lv_meter 全表重绘最贵，深降频消除帧率跳动） */
    if ((s_g_frame_cnt++ % 6u) == 0u) {
        int16_t pan_deg = (int16_t)((pan + G_PAN_MAX) / (2.0f * G_PAN_MAX) * 180.0f);
        int16_t tilt_deg = (int16_t)((tilt + G_TILT_MAX) / (2.0f * G_TILT_MAX) * 180.0f);
        if (s_g_pan_ind != NULL) {
            lv_meter_set_indicator_value(s_g_pan_meter, s_g_pan_ind, pan_deg);
        }
        if (s_g_tilt_ind != NULL) {
            lv_meter_set_indicator_value(s_g_tilt_meter, s_g_tilt_ind, tilt_deg);
        }
        lv_label_set_text_fmt(s_g_pan_val, "%+.1f°", (double)pan);
        lv_label_set_text_fmt(s_g_tilt_val, "%+.1f°", (double)tilt);
    }

    /* HUD：DEMO 显示跟踪偏差；REAL 显示当前通道姿态值 */
    if (s_g_mode == 0u) {
        int32_t dx = (int32_t)(tx - s_g_follow_x);
        int32_t dy = (int32_t)(ty - s_g_follow_y);
        lv_label_set_text_fmt(s_g_dx, "dx %+04d", (int)dx);
        lv_label_set_text_fmt(s_g_dy, "dy %+04d", (int)dy);
        GuiTheme_DotSet(s_g_track_dot,
                        (dx * dx + dy * dy) < 900 ? GUI_STATE_OK : GUI_STATE_WARN);
    } else {
        lv_label_set_text_fmt(s_g_dx, "r%+.1f", (double)pan);
        lv_label_set_text_fmt(s_g_dy, "p%+.1f", (double)tilt);
        GuiTheme_DotSet(s_g_track_dot, GUI_STATE_OK);
    }

    /* 标题栏：通道名 + fps + 渲染耗时 */
    uint32_t now = lv_tick_get();
    uint32_t dt = now - s_g_last_ms;
    s_g_last_ms = now;
    if (dt > 0u) {
        uint32_t fps = 1000u / dt;
        uint32_t us = g_gui_render_us;
        lv_label_set_text_fmt(s_g_perf, "%s %ufps %.1fms",
                              s_g_mode_name[s_g_mode], (unsigned)fps,
                              (double)us / 1000.0);
    }
}

/* 通道切换按钮：DEMO → RAW → COMP → KF → PID → DEMO
 * 长按（PID 模式内）：切换 PID 运行模式 STEP → SINE → DIST */
static void gimbal_ch_click(lv_event_t *e)
{
    uint32_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        if (s_g_mode == 4u) {
            s_pid_run_mode = (uint8_t)((s_pid_run_mode + 1u) % 3u);
            s_cv_n = 0;
            s_pid_t = 0.0f;
            s_plant_y = 0.0f;
            s_plant_ydot = 0.0f;
            s_plant_u_d[0] = s_plant_u_d[1] = s_plant_u_d[2] = 0.0f;
        }
        return;
    }
    if (code != LV_EVENT_CLICKED) {
        return;
    }
    s_g_mode = (uint8_t)((s_g_mode + 1u) % 5u);
    if (s_g_ch_btn != NULL) {
        lv_obj_t *lab = lv_obj_get_child(s_g_ch_btn, 0);
        if (lab != NULL) {
            lv_label_set_text(lab, s_g_mode_name[s_g_mode]);
        }
    }
    /* PID 模式：曲线 + 参数卡显示，仪表/目标/准星隐藏 */
    if (s_g_mode == 4u) {
        lv_obj_clear_flag(s_g_curve_set, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_g_curve_y, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_g_curve_u, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_g_kp_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_g_ki_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_g_pan_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_g_tilt_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_g_target, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_g_cross_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_g_track_dot, LV_OBJ_FLAG_HIDDEN);
        s_cv_n = 0;
        s_pid_t = 0.0f;
        s_plant_y = 0.0f;
        s_plant_ydot = 0.0f;
        s_plant_u_d[0] = s_plant_u_d[1] = s_plant_u_d[2] = 0.0f;
        s_pid_run_mode = 0;
    } else {
        lv_obj_add_flag(s_g_curve_set, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_g_curve_y, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_g_curve_u, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_g_kp_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_g_ki_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_g_pan_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_g_tilt_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_g_cross_canvas, LV_OBJ_FLAG_HIDDEN);
        /* 切到 REAL 通道：准星直接定位到当前姿态（避免从 DEMO 位置跳变追过去） */
        if (s_g_mode != 0u) {
            const imu_svc_state_t *imu = ImuSvc_GetState();
            float r, p;
            if (s_g_mode == 1u) { r = imu->acc_roll;  p = imu->acc_pitch; }
            else if (s_g_mode == 2u) { r = imu->comp_roll; p = imu->comp_pitch; }
            else { r = imu->roll; p = imu->pitch; }
            s_g_follow_x = G_FOV_W * 0.5f + (r / G_PAN_MAX) * G_FOV_W * 0.42f;
            s_g_follow_y = G_FOV_H * 0.5f - (p / G_TILT_MAX) * G_FOV_H * 0.40f;
        }
    }
}

/* ================================================================
 * 页面切换（方向动画：向右导航 MOVE_LEFT，向左 MOVE_RIGHT）
 * ================================================================ */
/* ---------------- 页面切换（方向动画：向右导航 MOVE_LEFT，向左 MOVE_RIGHT） ----------------
 * 快速连点保护：lv_scr_load_anim 无完成回调，连续切换会打断进行中的
 * 动画——旧屏残留为 scr_prev（LVGL 仍视为可见并渲染），两页内容叠加
 * （实测 HOME 与 NET 子栏重合，见 ENGINEERING_LOG 13.4）。
 * 切换动画期间屏蔽新请求（80ms 动画 + 余量 = 200ms），杜绝打断。 */
static lv_obj_t *s_active;
static uint8_t s_page_busy;

static void page_busy_clear(lv_timer_t *t)
{
    (void)t;
    s_page_busy = 0;
}

static void page_show(lv_obj_t *scr, lv_scr_load_anim_t dir)
{
    if (scr == s_active || scr == NULL || s_page_busy) {
        return;
    }
    s_active = scr;
    s_page_busy = 1;
    /* 动画 80ms：小屏切换轻快（原 150ms 在 60Hz 刷新下整页移动 9 帧，
     * 期间全屏重绘叠加触摸采样延迟，产生切换卡顿感） */
    lv_scr_load_anim(scr, dir, 80, 0, false);
    /* 一次性定时器：200ms 后解除切换屏蔽（动画完成后可再次切换） */
    lv_timer_t *t = lv_timer_create(page_busy_clear, 200, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/* GIMBAL 动画定时器回调：仅页面可见时驱动演示（s_active 本段已定义） */
static void gimbal_anim_cb(lv_timer_t *tmr)
{
    (void)tmr;
    if (s_active != s_scr_gimbal) {
        return;
    }
    page_gimbal_refresh();
}

void GuiPages_ShowHome(void) { page_show(s_scr_home, LV_SCR_LOAD_ANIM_MOVE_RIGHT); }
void GuiPages_ShowNet(void)  { page_show(s_scr_net,  LV_SCR_LOAD_ANIM_MOVE_LEFT); }
void GuiPages_ShowCam(void)  { page_show(s_scr_cam,  LV_SCR_LOAD_ANIM_MOVE_LEFT); }
void GuiPages_ShowGimbal(void) { page_show(s_scr_gimbal, LV_SCR_LOAD_ANIM_MOVE_LEFT); }
void GuiPages_ShowSys(void)  { page_show(s_scr_sys,  LV_SCR_LOAD_ANIM_MOVE_LEFT); }

/* 单按键/挥手翻页：按当前活动页轮换（含 CAM/GIMBAL 页） */
void GuiPages_PageNext(void)
{
    if (s_active == s_scr_home) {
        page_show(s_scr_net, LV_SCR_LOAD_ANIM_MOVE_LEFT);
    } else if (s_active == s_scr_net) {
        page_show(s_scr_cam, LV_SCR_LOAD_ANIM_MOVE_LEFT);
    } else if (s_active == s_scr_cam) {
        page_show(s_scr_gimbal, LV_SCR_LOAD_ANIM_MOVE_LEFT);
    } else if (s_active == s_scr_gimbal) {
        page_show(s_scr_sys, LV_SCR_LOAD_ANIM_MOVE_LEFT);
    } else {
        page_show(s_scr_home, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    }
}

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
    page_cam_build();
    page_gimbal_build();
    s_active = s_scr_home;
    lv_scr_load(s_scr_home);
    gui_data_collect();   /* 首窗立即采集（CPU 基线等） */
}

/* ---------------- 250ms 三相轮转刷新（彻底错峰） ----------------
 * 每 250ms 执行一相，3 相 = 750ms 完整刷一遍全部页面：
 *   相 0：数据采集 + 主页摘要/时钟 + 吞吐曲线
 *   相 1：主页卡片 0-2 + 网络页文本
 *   相 2：主页卡片 3-5 + 系统页文本（含 CPU 环/堆条动画值）
 * 每相仅 3-4 个控件变化（LVGL 相同文本自动跳过重绘），刷新帧轻量；
 * 数据采集粒度 250ms → 数值更新更"活"，配合环/条动画视觉平滑。 */
static uint8_t s_refr_phase;

void GuiPages_RefreshFast(void)
{
    switch (s_refr_phase) {
    case 0:
        gui_data_collect();
        page_home_refresh_top();
        page_net_curve();
        break;
    case 1:
        page_home_refresh_cards(0);
        page_net_refresh();
        break;
    default:
        page_home_refresh_cards(1);
        page_sys_refresh();
        break;
    }
    /* CAM 页 250ms 实时刷新（仅可见时；标签开销小，保证验证跟手） */
    if (s_active == s_scr_cam) {
        page_cam_refresh();
    }
    /* GIMBAL 页动画由独立 50ms 定时器驱动（gimbal_anim_cb），
     * 不走三相节拍（250ms = 4fps 会卡顿） */
    s_refr_phase = (uint8_t)((s_refr_phase + 1u) % 3u);
}
