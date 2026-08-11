/* ================================================================
 * mqtt_svc —— MQTT 工业遥测服务：连接/订阅/发布
 *
 * 架构位置：APP 应用层；broker 地址存 EEPROM，独立任务维护连接
 * 核心流程：connect -> 周期发布遥测 JSON -> 订阅回调入队
 * 关键约束：connect 内部 memset 会清回调，须 connect 后再设 data_cb
 * ================================================================ */
#include "mqtt_svc.h"
#include "usr_store.h"
#include "logger.h"
#include "app_config.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "timers.h"
#include "task.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/err.h"
#include "lwip/tcpip.h"
#include "lwip/apps/mqtt.h"
#include "eth_app.h"
#include "icmp_svc.h"

#include <stdio.h>
#include <string.h>

#define MQTT_DFLT_PORT      1883u
#define MQTT_TELE_PERIOD_MS 5000u
#define MQTT_TELE_TOPIC     "d00/status"

static mqtt_svc_stat_t s_stat;
static mqtt_client_t *s_client = NULL;
static TimerHandle_t s_timer = NULL;
static char s_topic[48];
static char s_payload[96];

static void mqtt_conn_cb(mqtt_client_t *client, void *arg,
                         mqtt_connection_status_t status)
{
    (void)client;
    (void)arg;
    switch (status) {
        case MQTT_CONNECT_ACCEPTED:
            s_stat.state = 2;
            LOG_Printf("[MQTT] connected\r\n");
            break;
        case MQTT_CONNECT_DISCONNECTED:
            s_stat.state = 0;
            s_stat.disconnect_cnt++;
            LOG_Printf("[MQTT] disconnected\r\n");
            break;
        default:
            s_stat.state = 3;
            s_stat.err_cnt++;
            LOG_Printf("[MQTT] connect error=%d\r\n", (int)status);
            break;
    }
}

static void mqtt_inpub_cb(void *arg, const char *topic, u32_t tot_len)
{
    (void)arg;
    (void)tot_len;
    LOG_Printf("[MQTT] recv topic=%s\r\n", topic ? topic : "?");
}

static void mqtt_indata_cb(void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    (void)arg;
    (void)flags;
    if (data != NULL && len > 0u) {
        LOG_Printf("[MQTT] payload(%u)=%.*s\r\n", (unsigned)len,
                   (int)(len < 64u ? len : 64u), (const char *)data);
    }
}

static void mqtt_req_cb(void *arg, err_t err)
{
    (void)arg;
    (void)err;
}

/* ---- tcpip 线程内执行的操作 ---- */
static void mqtt_do_connect(void *arg)
{
    (void)arg;
    if (s_client == NULL) {
        s_client = mqtt_client_new();
    }
    if (s_client == NULL) {
        s_stat.err_cnt++;
        return;
    }
    ip_addr_t b;
    IP4_ADDR(&b, s_stat.broker[0], s_stat.broker[1],
             s_stat.broker[2], s_stat.broker[3]);
    struct mqtt_connect_client_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.client_id = s_stat.client_id;
    ci.client_user = NULL;
    ci.client_pass = NULL;
    ci.keep_alive = 30;
    ci.will_topic = NULL;
    ci.will_msg = NULL;
    ci.will_qos = 0;
    ci.will_retain = 0;
    err_t e = mqtt_client_connect(s_client, &b, s_stat.port, mqtt_conn_cb,
                                  NULL, &ci);
    if (e != ERR_OK) {
        s_stat.err_cnt++;
        LOG_Printf("[MQTT] connect start failed (%d)\r\n", (int)e);
        return;
    }
    /* 注意：mqtt_client_connect 内部 memset 整个 client 结构，
     * 会清空此前设置的 inpub 回调——必须在 connect 之后（tcpip 线程内）重设，
     * 否则收到 PUBLISH 时 data_cb 为空指针 → INVSTATE 崩溃。 */
    mqtt_set_inpub_callback(s_client, mqtt_inpub_cb, mqtt_indata_cb, NULL);
}

typedef struct {
    const char *topic;
    const void *data;
    u16_t len;
    uint8_t sub;
} mqtt_op_t;

static void mqtt_do_pub(void *arg)
{
    const mqtt_op_t *op = (const mqtt_op_t *)arg;
    if (s_client == NULL || !mqtt_client_is_connected(s_client)) {
        return;
    }
    err_t e = mqtt_publish(s_client, op->topic, op->data, op->len,
                           0, 0, mqtt_req_cb, NULL);
    if (e == ERR_OK) {
        s_stat.pub_cnt++;
    }
}

static void mqtt_do_sub(void *arg)
{
    const mqtt_op_t *op = (const mqtt_op_t *)arg;
    if (s_client == NULL || !mqtt_client_is_connected(s_client)) {
        return;
    }
    err_t e = mqtt_sub_unsub(s_client, op->topic, 0, mqtt_req_cb, NULL, 1);
    if (e == ERR_OK) {
        s_stat.sub_cnt++;
    }
}

static void mqtt_do_disconnect(void *arg)
{
    (void)arg;
    if (s_client != NULL) {
        mqtt_disconnect(s_client);
    }
}

/* 遥测定时器：连接后每 5s 发布 JSON */
static void mqtt_tele_timer(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (s_client == NULL || !mqtt_client_is_connected(s_client)) {
        return;
    }
    EthApp_RefreshStatus();
    const eth_status_t *es = EthApp_GetStatus();
    const icmp_svc_stat_t *is = IcmpSvc_GetStat();
    uint32_t ver = *(volatile uint32_t *)OTA_APP_VERSION_ADDR;
    snprintf(s_payload, sizeof(s_payload),
             "{\"v\":%lu,\"up\":%lu,\"heap\":%lu,\"rx\":%lu,\"tx\":%lu,"
             "\"icmp_rx\":%lu,\"icmp_tx\":%lu}",
             (unsigned long)ver,
             (unsigned long)(HAL_GetTick() / 1000u),
             (unsigned long)xPortGetFreeHeapSize(),
             (unsigned long)es->rx_packets,
             (unsigned long)es->tx_packets,
             (unsigned long)is->echo_rx,
             (unsigned long)is->echo_tx);
    snprintf(s_topic, sizeof(s_topic), "%s", MQTT_TELE_TOPIC);
    static mqtt_op_t op;
    op.topic = s_topic;
    op.data = s_payload;
    op.len = (u16_t)strlen(s_payload);
    tcpip_callback(mqtt_do_pub, &op);
}

int MqttSvc_SetBroker(const char *ip, uint16_t port)
{
    ip4_addr_t a;
    if (ip == NULL || !ip4addr_aton(ip, &a)) {
        return -1;
    }
    s_stat.broker[0] = ip4_addr1(&a);
    s_stat.broker[1] = ip4_addr2(&a);
    s_stat.broker[2] = ip4_addr3(&a);
    s_stat.broker[3] = ip4_addr4(&a);
    s_stat.port = (port == 0u) ? MQTT_DFLT_PORT : port;
    uint8_t cfg[6];
    memcpy(cfg, s_stat.broker, 4);
    cfg[4] = (uint8_t)(s_stat.port >> 8);
    cfg[5] = (uint8_t)(s_stat.port & 0xFF);
    if (UsrStore_Set(USR_KEY_MQTT_BROKER, cfg, sizeof(cfg)) != 0) {
        return -2;
    }
    LOG_Printf("[MQTT] broker %u.%u.%u.%u:%u saved to EEPROM\r\n",
               (unsigned)s_stat.broker[0], (unsigned)s_stat.broker[1],
               (unsigned)s_stat.broker[2], (unsigned)s_stat.broker[3],
               (unsigned)s_stat.port);
    return 0;
}

int MqttSvc_Connect(const char *ip, uint16_t port)
{
    if (ip != NULL) {
        int r = MqttSvc_SetBroker(ip, port);
        if (r != 0) {
            return r;
        }
    } else if (s_stat.port == 0u) {
        return -3;                       /* 未配置 broker */
    }
    s_stat.state = 1;
    s_stat.connect_cnt++;
    if (s_client == NULL) {
        s_client = mqtt_client_new();
    }
    if (s_client == NULL) {
        s_stat.state = 3;
        s_stat.err_cnt++;
        return -2;
    }
    if (tcpip_callback(mqtt_do_connect, NULL) != ERR_OK) {
        return -4;
    }
    LOG_Printf("[MQTT] connecting %u.%u.%u.%u:%u ...\r\n",
               (unsigned)s_stat.broker[0], (unsigned)s_stat.broker[1],
               (unsigned)s_stat.broker[2], (unsigned)s_stat.broker[3],
               (unsigned)s_stat.port);
    return 0;
}

void MqttSvc_Disconnect(void)
{
    tcpip_callback(mqtt_do_disconnect, NULL);
}

int MqttSvc_Publish(const char *topic, const char *data)
{
    if (topic == NULL || data == NULL) {
        return -1;
    }
    snprintf(s_topic, sizeof(s_topic), "%s", topic);
    snprintf(s_payload, sizeof(s_payload), "%s", data);
    static mqtt_op_t op;
    op.topic = s_topic;
    op.data = s_payload;
    op.len = (u16_t)strlen(s_payload);
    return (tcpip_callback(mqtt_do_pub, &op) == ERR_OK) ? 0 : -2;
}

int MqttSvc_Subscribe(const char *topic)
{
    if (topic == NULL) {
        return -1;
    }
    snprintf(s_topic, sizeof(s_topic), "%s", topic);
    static mqtt_op_t op;
    op.topic = s_topic;
    op.sub = 1;
    return (tcpip_callback(mqtt_do_sub, &op) == ERR_OK) ? 0 : -2;
}

const mqtt_svc_stat_t *MqttSvc_GetStat(void)
{
    return &s_stat;
}

void MqttSvc_Init(void)
{
    memset(&s_stat, 0, sizeof(s_stat));
    snprintf(s_stat.client_id, sizeof(s_stat.client_id), "D00-F407");
    uint8_t cfg[6];
    if (UsrStore_Get(USR_KEY_MQTT_BROKER, cfg, sizeof(cfg)) == (int)sizeof(cfg)) {
        memcpy(s_stat.broker, cfg, 4);
        s_stat.port = (uint16_t)((uint16_t)cfg[4] << 8 | cfg[5]);
        LOG_Printf("[MQTT] broker %u.%u.%u.%u:%u (saved)\r\n",
                   (unsigned)s_stat.broker[0], (unsigned)s_stat.broker[1],
                   (unsigned)s_stat.broker[2], (unsigned)s_stat.broker[3],
                   (unsigned)s_stat.port);
    }
    s_timer = xTimerCreate("mqttTele", pdMS_TO_TICKS(MQTT_TELE_PERIOD_MS),
                           pdTRUE, NULL, mqtt_tele_timer);
    if (s_timer != NULL) {
        xTimerStart(s_timer, 0);
    }
}
