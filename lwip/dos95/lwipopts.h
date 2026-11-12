/* lwIP options for DOS95: single-threaded core driven by the net kernel thread */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H
#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_DHCP                       1
#define LWIP_AUTOIP                     0
#define LWIP_ACD                        0
#define LWIP_DHCP_DOES_ACD_CHECK        0
#define LWIP_DNS                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
/* v41 (#336): multicast - a program joins groups (IP_ADD_MEMBERSHIP: IGMPv2), sends to them with
 * its own TTL, loop and interface (until v41 none: the C library took the options and nothing was
 * done); a datagram to a group or a broadcast goes to every socket on its port that has
 * SO_REUSEADDR, as BSD's and Winsock's (lwIP's default: the first) */
#define LWIP_IGMP                       1
#define SO_REUSE_RXTOALL                1
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_STATS                      0
#ifdef QDP
/* the QEMU DOS port: lwIP's memory from the kernel's heap.  This lwIP's MEM_LIBC_MALLOC calls malloc, free and calloc
 * (the mem_clib_ names below are an older lwIP's) - QEMU's (qemu/dos95/libc.c), whose blocks the QEMU DOS port frees
 * when a machine's run is over: the network's buffers must not be among them */
void *lwip_kmalloc(unsigned n); void lwip_kfree(void *p); void *lwip_kcalloc(unsigned a, unsigned b);
#define MEM_LIBC_MALLOC                 0
#define MEM_CUSTOM_ALLOCATOR            1
#define MEM_CUSTOM_FREE                 lwip_kfree
#define MEM_CUSTOM_MALLOC               lwip_kmalloc
#define MEM_CUSTOM_CALLOC               lwip_kcalloc
#else
#define MEM_LIBC_MALLOC                 1
#endif
#define MEMP_MEM_MALLOC                 1
#define mem_clib_malloc                 lwip_kmalloc
#define mem_clib_free                   lwip_kfree
#define mem_clib_calloc                 lwip_kcalloc
#define MEM_ALIGNMENT                   4
#define PBUF_POOL_SIZE                  48
#define PBUF_POOL_BUFSIZE               1536
#define TCP_MSS                         1460
/* v41 (#336): a connection's windows as a machine of now has them - 256 KB each way, with window
 * scaling (RFC 7323; a peer without it gets 64 KB), not 11 KB (8 segments: 233 KB/s at 50 ms
 * there and back).  Memory is taken as data waits, not up front (MEMP_MEM_MALLOC below). */
#define LWIP_WND_SCALE                  1
#define TCP_RCV_SCALE                   2
#define TCP_WND                         (0xFFFFU << TCP_RCV_SCALE)
#define TCP_SND_BUF                     (256 * 1024)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF + TCP_MSS - 1) / TCP_MSS)
#define TCP_SNDLOWAT                    32768      /* (lwIP's socket API's select(); ours has its own) */
#define MEMP_NUM_TCP_PCB                16
#define MEMP_NUM_TCP_SEG                64
#define MEMP_NUM_UDP_PCB                8
#define MEMP_NUM_RAW_PCB                4
#define MEMP_NUM_PBUF                   32
#define LWIP_CHKSUM_ALGORITHM           2
#define ETHARP_SUPPORT_VLAN             0
#define LWIP_TCP_KEEPALIVE              1
#define TCP_LISTEN_BACKLOG              1
#define SO_REUSE                        1
#define LWIP_SO_RCVBUF                  0
#define LWIP_DBG_MIN_LEVEL              LWIP_DBG_LEVEL_ALL
#define LWIP_DEBUG                      0
#define DHCP_DEBUG                      LWIP_DBG_OFF
#define LWIP_TIMEVAL_PRIVATE            0
/* v37: 127.0.0.1 (lwIP's loop interface) and a program reaching its own address */
#define LWIP_NETIF_LOOPBACK             1
#define LWIP_HAVE_LOOPIF                1
#define LWIP_LOOPBACK_MAX_PBUFS         0          /* v41 (#336): no 256 - the windows bound it */
#define LWIP_NETIF_TX_SINGLE_PBUF       1
void *lwip_kmalloc(unsigned n); void lwip_kfree(void *p); void *lwip_kcalloc(unsigned a, unsigned b);
unsigned lwip_mem_room(void);                      /* a sixteenth of the free memory, in bytes (glue.c) */
/* v41 (#336): no fixed tables - as Windows 95's stack, which had none of these:
 *   the ARP table grows as hosts are talked to (etharp.c, DOS95_ARP_GROWS; lwIP's 10 entries:
 *   an eleventh host pushed one out), and packets wait for an address being asked for - as
 *   many as a sixteenth of the free memory holds (lwIP kept the last one only) */
#define DOS95_ARP_GROWS                 1
#define ARP_TABLE_SIZE                  0x7FFF     /* (the most it grows to: lwIP's 16-bit index - netif.h's netif_addr_idx_t) */
#define ARP_QUEUEING                    1
#define ARP_QUEUE_LEN                   0
#define DOS95_ARP_QUEUE_ROOM            lwip_mem_room()
/*   datagrams of any size are put together from their fragments (65535 bytes: 45 of them), as
 *   many at once as a sixteenth of the free memory holds (ip4_frag.c, DOS95_REASS_PBUFS; lwIP's
 *   10 pbufs: less than 14 KB) - 65535 is the count's own width (u16_t) */
#define IP_REASS_MAX_PBUFS              0xFFFF
#define DOS95_REASS_PBUFS               (lwip_mem_room() / PBUF_POOL_BUFSIZE > 45 ? lwip_mem_room() / PBUF_POOL_BUFSIZE : 45)
/*   names (dns.c, DOS95_DNS_GROWS): the table of names asked and answered, the requests waiting
 *   on them and their UDP ports grow (lwIP's 4: a fifth name asked while four waited failed at
 *   once); the name servers are asked in turn - as many as DHCP gives (its option holds 63) or
 *   Control Panel > Network lists: 255, the width of lwIP's index (u8_t) */
#define DOS95_DNS_GROWS                 1
#define DNS_MAX_SERVERS                 255
/*   DHCP's domain name (option 15) - the resolver's first suffix, as Windows 95's (glue.c) - and
 *   (v41, #362) its name servers (option 6): the glue's to set, not lwIP's - the servers Control
 *   Panel > Network lists (DOS95.INI DNS=, DNS2= ...) are asked, and DHCP's only when none is
 *   listed, as Windows 95 has it (its DNS Configuration's servers over the ones DHCP gives); until
 *   v41 DHCP's were the only ones while DHCP was on, the list set was not asked */
#define LWIP_DHCP_MAX_DNS_SERVERS       0
#define DHCP_ADD_EXTRA_REQUEST_OPTIONS  , 15, 6
void lw_dhcp_option(int msg_type, int option, int len, const void *pbuf, int offset);
#define LWIP_HOOK_DHCP_PARSE_OPTION(netif, dhcp, state, msg, msg_type, option, len, pbuf, offset) \
        lw_dhcp_option(msg_type, option, len, pbuf, offset)
#endif
