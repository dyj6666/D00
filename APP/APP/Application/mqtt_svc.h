/* ================================================================
 * MQTT 工业遥测服务（lwIP mqtt 客户端）
 *   - `mqtt connect <ip> [port]`（默认 1883，异步）/ `mqtt disconnect`
 *   - `mqtt pub <topic> <data>` / `mqtt sub <topic>` / `mqtt info`
 *   - 连接成功后每 5s 自动发布设备遥测 JSON（d00/status）
 *   - broker 地址持久化到 EEPROM（用户数据，USR_KEY_MQTT_BROKER）
 *   - 回调运行在 tcpip 线程；发布/订阅经 tcpip_callback 转发
 * ================================================================ */
#ifndef MQTT_SVC_H
#define MQTT_SVC_H

#include <stdint.h>

typedef struct {
    volatile uint8_t  state;      /* 0=IDLE 1=CONNECTING 2=CONNECTED 3=ERR */
    volatile uint32_t connect_cnt;
    volatile uint32_t disconnect_cnt;
    volatile uint32_t pub_cnt;
    volatile uint32_t sub_cnt;
    volatile uint32_t err_cnt;
    uint8_t  broker[4];
    uint16_t port;
    char     client_id[24];
} mqtt_svc_stat_t;

void MqttSvc_Init(void);
int  MqttSvc_SetBroker(const char *ip, uint16_t port);  /* 0=成功（写 EEPROM） */
int  MqttSvc_Connect(const char *ip, uint16_t port);    /* 异步发起；0=已启动 */
void MqttSvc_Disconnect(void);
int  MqttSvc_Publish(const char *topic, const char *data);
int  MqttSvc_Subscribe(const char *topic);
const mqtt_svc_stat_t *MqttSvc_GetStat(void);

#endif
