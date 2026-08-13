/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : Target/lwipopts.h
  * Description        : This file overrides LwIP stack default configuration
  *                      done in opt.h file.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion --------------------------------------*/
#ifndef __LWIPOPTS__H__
#define __LWIPOPTS__H__

#include "main.h"

/*-----------------------------------------------------------------------------*/
/* Current version of LwIP supported by CubeMx: 2.1.2 -*/
/*-----------------------------------------------------------------------------*/

/* Within 'USER CODE' section, code will be kept by default at each generation */
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

#ifdef __cplusplus
 extern "C" {
#endif

/* STM32CubeMX Specific Parameters (not defined in opt.h) ---------------------*/
/* Parameters set in STM32CubeMX LwIP Configuration GUI -*/
/*----- WITH_RTOS enabled (Since FREERTOS is set) -----*/
#define WITH_RTOS 1
/*----- CHECKSUM_BY_HARDWARE enabled -----*/
#define CHECKSUM_BY_HARDWARE 1
/*-----------------------------------------------------------------------------*/

/* LwIP Stack Parameters (modified compared to initialization value in opt.h) -*/
/* Parameters set in STM32CubeMX LwIP Configuration GUI -*/
/*----- Value in opt.h for LWIP_DHCP: 0 -----*/
#define LWIP_DHCP 1
/*----- Value in opt.h for MEM_ALIGNMENT: 1 -----*/
#define MEM_ALIGNMENT 4
/*----- Value in opt.h for MEMP_NUM_SYS_TIMEOUT: (LWIP_TCP + IP_REASSEMBLY + LWIP_ARP + (2*LWIP_DHCP) + LWIP_AUTOIP + LWIP_IGMP + LWIP_DNS + (PPP_SUPPORT*6*MEMP_NUM_PPP_PCB) + (LWIP_IPV6 ? (1 + LWIP_IPV6_REASS + LWIP_IPV6_MLD) : 0)) -*/
#define MEMP_NUM_SYS_TIMEOUT 5
/*----- Value in opt.h for LWIP_ETHERNET: LWIP_ARP || PPPOE_SUPPORT -*/
#define LWIP_ETHERNET 1
/*----- Value in opt.h for LWIP_DNS_SECURE: (LWIP_DNS_SECURE_RAND_XID | LWIP_DNS_SECURE_NO_MULTIPLE_OUTSTANDING | LWIP_DNS_SECURE_RAND_SRC_PORT) -*/
#define LWIP_DNS_SECURE 7
/*----- Value in opt.h for TCP_SND_QUEUELEN: (4*TCP_SND_BUF + (TCP_MSS - 1))/TCP_MSS -----*/
#define TCP_SND_QUEUELEN 9
/*----- Value in opt.h for TCP_SNDLOWAT: LWIP_MIN(LWIP_MAX(((TCP_SND_BUF)/2), (2 * TCP_MSS) + 1), (TCP_SND_BUF) - 1) -*/
#define TCP_SNDLOWAT 1071
/*----- Value in opt.h for TCP_SNDQUEUELOWAT: LWIP_MAX(TCP_SND_QUEUELEN)/2, 5) -*/
#define TCP_SNDQUEUELOWAT 5
/*----- Value in opt.h for TCP_WND_UPDATE_THRESHOLD: LWIP_MIN(TCP_WND/4, TCP_MSS*4) -----*/
#define TCP_WND_UPDATE_THRESHOLD 536
/*----- Value in opt.h for LWIP_NETIF_LINK_CALLBACK: 0 -----*/
#define LWIP_NETIF_LINK_CALLBACK 1
/*----- Value in opt.h for TCPIP_THREAD_STACKSIZE: 0 -----*/
#define TCPIP_THREAD_STACKSIZE 1024
/*----- Value in opt.h for TCPIP_THREAD_PRIO: 1 -----*/
/* tcpip 线程优先级 32（osPriorityAboveNormal）：高于所有 netconn 使用任务
 * （TcpSvc 24 / OtaTcpSvc 24 / HttpSvc 16），协议栈处理不被应用任务拖延 */
#define TCPIP_THREAD_PRIO 32
/*----- Value in opt.h for TCPIP_MBOX_SIZE: 0 -----*/
#define TCPIP_MBOX_SIZE 24   /* 突发下防止窗口更新消息被丢（HTTP 拉取实测 15s 停滞根因） */
/*----- Value in opt.h for SLIPIF_THREAD_STACKSIZE: 0 -----*/
#define SLIPIF_THREAD_STACKSIZE 1024
/*----- Value in opt.h for SLIPIF_THREAD_PRIO: 1 -----*/
#define SLIPIF_THREAD_PRIO 3
/*----- Value in opt.h for DEFAULT_THREAD_STACKSIZE: 0 -----*/
#define DEFAULT_THREAD_STACKSIZE 1024
/*----- Value in opt.h for DEFAULT_THREAD_PRIO: 1 -----*/
#define DEFAULT_THREAD_PRIO 3
/*----- Value in opt.h for DEFAULT_UDP_RECVMBOX_SIZE: 0 -----*/
#define DEFAULT_UDP_RECVMBOX_SIZE 6
/*----- Value in opt.h for DEFAULT_TCP_RECVMBOX_SIZE: 0 -----*/
#define DEFAULT_TCP_RECVMBOX_SIZE 12
/*----- Value in opt.h for DEFAULT_ACCEPTMBOX_SIZE: 0 -----*/
#define DEFAULT_ACCEPTMBOX_SIZE 6
/*----- Value in opt.h for RECV_BUFSIZE_DEFAULT: INT_MAX -----*/
#define RECV_BUFSIZE_DEFAULT 2000000000
/*----- Value in opt.h for LWIP_STATS: 1 -----*/
#define LWIP_STATS 0
/*----- Value in opt.h for CHECKSUM_GEN_IP: 1 -----*/
#define CHECKSUM_GEN_IP 0
/*----- Value in opt.h for CHECKSUM_GEN_UDP: 1 -----*/
#define CHECKSUM_GEN_UDP 0
/*----- Value in opt.h for CHECKSUM_GEN_TCP: 1 -----*/
#define CHECKSUM_GEN_TCP 0
/*----- Value in opt.h for CHECKSUM_GEN_ICMP: 1 -----*/
#define CHECKSUM_GEN_ICMP 0
/*----- Value in opt.h for CHECKSUM_GEN_ICMP6: 1 -----*/
#define CHECKSUM_GEN_ICMP6 0
/*----- Value in opt.h for CHECKSUM_CHECK_IP: 1 -----*/
#define CHECKSUM_CHECK_IP 0
/*----- Value in opt.h for CHECKSUM_CHECK_UDP: 1 -----*/
#define CHECKSUM_CHECK_UDP 0
/*----- Value in opt.h for CHECKSUM_CHECK_TCP: 1 -----*/
#define CHECKSUM_CHECK_TCP 0
/*----- Value in opt.h for CHECKSUM_CHECK_ICMP: 1 -----*/
#define CHECKSUM_CHECK_ICMP 0
/*----- Value in opt.h for CHECKSUM_CHECK_ICMP6: 1 -----*/
#define CHECKSUM_CHECK_ICMP6 0
/*-----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */
/* ---- 性能/内存平衡调优（覆盖 opt.h 默认，CubeMX 重新生成后保留） ----
 * 零拷贝 RX（自定义池 8×1536）+ 硬件 TX 校验和卸载；
 * TCP 窗口 8.5KB / 发送缓冲 4.3KB，MEM 堆 12KB 覆盖双连接。 */
#define MEM_SIZE                    (12 * 1024)
#define PBUF_POOL_SIZE              32   /* 突发吸收：HTTP 拉取实测停滞点=接收缓冲总容量 */
/* TCP_MSS 必须显式定义：缺省 536 会使窗口仅 4.3KB，8KB 突发直接超窗
 * 造成客户端 sendall 与板端发送缓冲双向死锁（实测 OTA 服务卡死）。 */
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#undef  TCP_SND_QUEUELEN
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS + 8)
#undef  TCP_SNDLOWAT
#define TCP_SNDLOWAT                (TCP_SND_BUF / 2)
#undef  TCP_SNDQUEUELOWAT
#define TCP_SNDQUEUELOWAT           12
#undef  TCP_WND_UPDATE_THRESHOLD
#define TCP_WND_UPDATE_THRESHOLD    (TCP_WND / 2)
#define MEMP_NUM_TCP_SEG            64
#define MEMP_NUM_ARP_QUEUE          16
#define MEMP_NUM_NETBUF             8    /* netconn_recv 并发 netbuf 需求 */
#define LWIP_RAW                    1   /* ICMP ping（raw API） */
#define LWIP_SOCKET                 0   /* 未用 POSIX socket：省 flash，服务用 netconn/raw */
#define LWIP_DNS                    1   /* DNS 客户端（net dns resolve） */
#define LWIP_MQTT                   1   /* MQTT 客户端（mqtt 服务，工业遥测） */
#undef  MEMP_NUM_SYS_TIMEOUT
#define MEMP_NUM_SYS_TIMEOUT        8   /* TCP+ARP+DHCP×2+DNS+MQTT 心跳等 */
#undef  CHECKSUM_GEN_ICMP
#define CHECKSUM_GEN_ICMP           1   /* F4 MAC 不卸载 ICMP 校验和，必须软件计算 */
#undef  CHECKSUM_GEN_IP
#define CHECKSUM_GEN_IP             1   /* 对照实验：软件计算，排除 MAC 卸载嫌疑 */
#undef  CHECKSUM_GEN_UDP
#define CHECKSUM_GEN_UDP            1
#undef  CHECKSUM_GEN_TCP
#define CHECKSUM_GEN_TCP            1
#undef  TCPIP_THREAD_STACKSIZE
#define TCPIP_THREAD_STACKSIZE      2048   /* raw 回调（ping/回显）在 tcpip 线程执行，1024 会溢出 */
#undef  DEFAULT_THREAD_STACKSIZE
#define DEFAULT_THREAD_STACKSIZE    2048
#define LWIP_SO_RCVTIMEO            1      /* netconn_set_recvtimeout（TCP 控制台空闲/遥测超时） */
/* USER CODE END 1 */

#ifdef __cplusplus
}
#endif
#endif /*__LWIPOPTS__H__ */
