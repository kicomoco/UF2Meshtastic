// lwIP configuration: NO_SYS polling mode (pico_cyw43_arch_lwip_poll), sized
// for one HTTP(S) connection at a time to keep RAM use down.
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0

// lwIP heap allocations (including mbedTLS buffers via altcp_tls) use malloc.
#define MEM_LIBC_MALLOC 1
#define MEM_ALIGNMENT 4
// Only an upper bound for single allocations when MEM_LIBC_MALLOC is set.
#define MEM_SIZE 24000

#define MEMP_NUM_TCP_SEG 16
#define MEMP_NUM_ARP_QUEUE 4
#define MEMP_NUM_TCP_PCB 2
#define MEMP_NUM_UDP_PCB 3
#define PBUF_POOL_SIZE 8

#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 0
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DNS 1
#define LWIP_DHCP 1
#define DHCP_DOES_ARP_CHECK 0
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_TCP_KEEPALIVE 1

#define TCP_MSS 1460
// altcp_tls only reopens the window once a whole TLS record is decrypted, and
// lwIP won't advertise a window below one MSS. The window must therefore hold
// a full 16 KB record plus an MSS or servers sending large records deadlock.
// Received pbufs are freed as mbedTLS copies them, so this costs little RAM.
#define TCP_WND (14 * TCP_MSS)
#define LWIP_DISABLE_TCP_SANITY_CHECKS 1
#define TCP_SND_BUF (2 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define LWIP_CHKSUM_ALGORITHM 3

#define LWIP_ALTCP 1
#define LWIP_ALTCP_TLS 1
#define LWIP_ALTCP_TLS_MBEDTLS 1

#define MEM_STATS 0
#define SYS_STATS 0
#define MEMP_STATS 0
#define LINK_STATS 0
#define LWIP_STATS 0

#endif
