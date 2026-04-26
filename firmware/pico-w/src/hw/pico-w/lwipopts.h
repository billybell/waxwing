#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                          1
#define LWIP_DHCP                       0
#define LWIP_AUTOIP                     0
#define LWIP_IGMP                       0
#define LWIP_TCP                        0
#define LWIP_UDP                        0
#define LWIP_DNS                        0
#define LWIP_RAW                        0
#define MEM_LIBC_MALLOC                 0
#define MEMP_SEPARATE_POOLS             0
#define LWIP_STATS                      0
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0
#define LWIP_SERIALIZE                  0
#define MEM_SIZE                        1000
#define MEM_ALIGNMENT                   4
#define MEMP_NUM_PBUF                   10
#define MEMP_NUM_RAW_PCB                0
#define MEMP_NUM_UDP_PCB                0
#define MEMP_NUM_TCP_PCB                0
#define MEMP_NUM_TCP_SEG                0
#define PBUF_POOL_SIZE                  0
#define PBUF_POOL_BUFSIZE               100
#define LWIP_SINGLE_NETIF               1
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_LINK_CALLBACK        0

#endif
