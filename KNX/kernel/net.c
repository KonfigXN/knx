#include "knx.h"

static void *mcpy(void *d, const void *s, uint64_t n) {
    uint8_t *a = (uint8_t *)d;
    const uint8_t *b = (const uint8_t *)s;
    for (uint64_t i = 0; i < n; ++i)
        a[i] = b[i];
    return d;
}
static void *mset(void *d, int v, uint64_t n) {
    uint8_t *a = (uint8_t *)d;
    for (uint64_t i = 0; i < n; ++i)
        a[i] = (uint8_t)v;
    return d;
}

#define ETH_ALEN 6
#define ETH_HDR_LEN 14
#define ETH_TYPE_ARP 0x0806
#define ETH_TYPE_IP  0x0800

#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17

static uint32_t net_ip = 0x0F02000A;
static uint32_t net_mask = 0x00FFFFFF;
static uint32_t net_gw = 0x0202000A;

static uint32_t htonl32(uint32_t v) {
    return ((v & 0xff) << 24) | (((v >> 8) & 0xff) << 16) |
           (((v >> 16) & 0xff) << 8) | ((v >> 24) & 0xff);
}

static uint16_t ip_checksum(const void *data, uint64_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    for (uint64_t i = 0; i + 1 < len; i += 2)
        sum += ((uint16_t)p[i] << 8) | p[i + 1];
    if (len & 1)
        sum += (uint16_t)p[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

typedef struct knx_netdev_ops {
    int  (*send)(const void *frame, uint64_t len);
    void (*poll)(void);
    int  (*link_up)(void);
} knx_netdev_ops;

typedef struct knx_netdev {
    const char *name;
    uint8_t mac[ETH_ALEN];
    const knx_netdev_ops *ops;
    int present;
} knx_netdev;

static knx_netdev g_netdev;

static void net_handle_packet(const uint8_t *pkt, uint64_t len);

static int netdev_register(const char *name, const uint8_t *mac,
                           const knx_netdev_ops *ops) {
    if (g_netdev.present || !name || !mac || !ops || !ops->send || !ops->poll)
        return 0;
    g_netdev.name = name;
    mcpy(g_netdev.mac, mac, ETH_ALEN);
    g_netdev.ops = ops;
    g_netdev.present = 1;
    return 1;
}

static int net_send_frame(const void *data, uint64_t len) {
    if (!g_netdev.present || !g_netdev.ops->send)
        return -1;
    return g_netdev.ops->send(data, len);
}

static void net_poll(void) {
    if (g_netdev.present && g_netdev.ops->poll)
        g_netdev.ops->poll();
}

void knx_net_tick(void) {
    net_poll();
}

static int  rtl_send(const void *frame, uint64_t len);
static void rtl_poll(void);
static int  rtl_link_up(void);
static int  e1000_send(const void *frame, uint64_t len);
static void e1000_poll(void);
static int  e1000_link_up(void);

static const knx_netdev_ops rtl_ops = { rtl_send, rtl_poll, rtl_link_up };
static const knx_netdev_ops e1000_ops = { e1000_send, e1000_poll, e1000_link_up };

#define RTL_MAC0     0x00
#define RTL_TSD0     0x10
#define RTL_TSAD0    0x20
#define RTL_RBSTART  0x30
#define RTL_CAPR     0x38
#define RTL_CBR      0x3A
#define RTL_IMR      0x3C
#define RTL_ISR      0x3E
#define RTL_TCR      0x40
#define RTL_RCR      0x44
#define RTL_CONFIG1  0x52
#define RTL_MSR      0x58
#define RTL_CR       0x37

#define RTL_CR_RESET 0x10
#define RTL_CR_RXEN  0x08
#define RTL_CR_TXEN  0x04
#define RTL_CR_BUFE  0x01

#define RTL_ISR_TOK  0x04
#define RTL_ISR_TER  0x08

#define RTL_MSR_LINKFAIL 0x04

#define RTL_RX_RING 8192u

static uint8_t rtl_rxbuf[RTL_RX_RING + 16] __attribute__((aligned(16)));
static uint8_t rtl_txbuf[1514 + 8] __attribute__((aligned(16)));
static uint16_t rtl_io;
static uint16_t rtl_rxread;
static uint8_t rtl_mac[ETH_ALEN];
static int rtl_present;

static int rtl_link_up(void) {
    if (!rtl_present || !rtl_io)
        return 0;
    return !(knx_io_in8(rtl_io + RTL_MSR) & RTL_MSR_LINKFAIL);
}

static int rtl_send(const void *data, uint64_t len) {
    if (!rtl_present || !data || len == 0 || len > 1514)
        return -1;

    unsigned n = (unsigned)len;

    if (n < 60)
        n = 60;
    for (unsigned i = 0; i < n; ++i)
        rtl_txbuf[i] = (i < len) ? ((const uint8_t *)data)[i] : 0;

    knx_io_out16(rtl_io + RTL_ISR, RTL_ISR_TOK | RTL_ISR_TER);
    knx_io_out32(rtl_io + RTL_TSAD0, (uint32_t)(uintptr_t)rtl_txbuf);
    knx_io_out32(rtl_io + RTL_TSD0, n);

    for (unsigned i = 0; i < 200000; ++i) {
        uint16_t isr = knx_io_in16(rtl_io + RTL_ISR);
        uint16_t ack = (uint16_t)(isr & (RTL_ISR_TOK | RTL_ISR_TER));
        if (!ack)
            continue;
        knx_io_out16(rtl_io + RTL_ISR, ack);
        if (ack & RTL_ISR_TER)
            return -1;
        if (ack & RTL_ISR_TOK)
            return (int)len;
    }
    return -1;
}

static void rtl_poll(void) {
    if (!rtl_present || !rtl_io)
        return;

    for (unsigned guard = 0; guard < 512; ++guard) {
        if (knx_io_in8(rtl_io + RTL_CR) & RTL_CR_BUFE)
            break;
        if ((uint32_t)rtl_rxread + 4u > RTL_RX_RING) {
            rtl_rxread = 0;
            knx_io_out16(rtl_io + RTL_CAPR, 0xFFF0);
            break;
        }

        uint16_t status = *(const volatile uint16_t *)(rtl_rxbuf + rtl_rxread);
        uint16_t rxsize = *(const volatile uint16_t *)(rtl_rxbuf + rtl_rxread + 2);

        if (rxsize >= 4 && rxsize <= 2048 &&
            (status & 0x0001) && !(status & 0x003E)) {
            net_handle_packet(rtl_rxbuf + rtl_rxread + 4, (uint64_t)(rxsize - 4));
        }

        rtl_rxread = (uint16_t)((rtl_rxread + rxsize + 4 + 3) & ~3u);
        if (rxsize < 4 || rxsize > 2048)
            rtl_rxread = 0;
        if (rtl_rxread >= RTL_RX_RING)
            rtl_rxread = (uint16_t)(rtl_rxread - RTL_RX_RING);

        uint32_t capr = rtl_rxread ? ((uint32_t)rtl_rxread - 0x10u) : 0xFFF0u;
        knx_io_out16(rtl_io + RTL_CAPR, (uint16_t)capr);
    }
}

int knx_rtl8139_probe(const knx_pci_device *device) {
    if (!device || g_netdev.present || rtl_present)
        return 0;
    if (device->vendor_id != 0x10EC ||
        (device->device_id != 0x8139 && device->device_id != 0x8138))
        return 0;

    uint32_t bar = device->bar[0];
    if (!(bar & 1u))
        bar = device->bar[1];
    uint32_t io32 = bar & ~3u;
    if (!(bar & 1u) || io32 == 0 || io32 > 0xffffu)
        return 0;
    uint16_t io = (uint16_t)io32;

    if (knx_pci_enable_device(device, KNX_PCI_ENABLE_IO | KNX_PCI_ENABLE_BUS_MASTER) != 0)
        return 0;

    rtl_io = io;

    knx_io_out8(io + RTL_CONFIG1, 0x00);

    knx_io_out8(io + RTL_CR, RTL_CR_RESET);
    for (unsigned i = 0; i < 100000; ++i) {
        if (!(knx_io_in8(io + RTL_CR) & RTL_CR_RESET))
            break;
    }
    if (knx_io_in8(io + RTL_CR) & RTL_CR_RESET) {
        rtl_io = 0;
        return 0;
    }

    for (unsigned i = 0; i < ETH_ALEN; ++i)
        rtl_mac[i] = knx_io_in8(io + RTL_MAC0 + i);

    mset(rtl_rxbuf, 0, sizeof(rtl_rxbuf));
    knx_io_out32(io + RTL_RBSTART, (uint32_t)(uintptr_t)rtl_rxbuf);
    knx_io_out16(io + RTL_CAPR, 0);
    knx_io_out16(io + RTL_IMR, 0);

    knx_io_out32(io + RTL_TCR, 0x03000700u);
    knx_io_out32(io + RTL_RCR, 0x0000008Fu);
    knx_io_out8(io + RTL_CR, RTL_CR_RXEN | RTL_CR_TXEN);

    rtl_rxread = 0;
    rtl_present = 1;
    netdev_register("RTL8139", rtl_mac, &rtl_ops);
    knx_log("[RTL8139]", "Initialized");
    return 1;
}

#define E1000_CTRL   0x0000
#define E1000_STATUS 0x0008
#define E1000_ICR    0x00C0
#define E1000_IMC    0x00D8
#define E1000_RCTL   0x0100
#define E1000_TCTL   0x0400
#define E1000_TIPG   0x0410
#define E1000_RDBAL  0x2800
#define E1000_RDBAH  0x2804
#define E1000_RDLEN  0x2808
#define E1000_RDH    0x2810
#define E1000_RDT    0x2818
#define E1000_TDBAL  0x3800
#define E1000_TDBAH  0x3804
#define E1000_TDLEN  0x3808
#define E1000_TDH    0x3810
#define E1000_TDT    0x3818
#define E1000_MTA    0x5200
#define E1000_RAL    0x5400
#define E1000_RAH    0x5404

#define E_CTRL_RST   (1u << 26)
#define E_CTRL_SLU   (1u << 6)
#define E_CTRL_ASDE  (1u << 5)
#define E_STATUS_LU  (1u << 1)

#define E_RCTL_EN    (1u << 1)
#define E_RCTL_UPE   (1u << 3)
#define E_RCTL_MPE   (1u << 4)
#define E_RCTL_BAM   (1u << 15)
#define E_RCTL_SECRC (1u << 26)

#define E_TCTL_EN    (1u << 1)
#define E_TCTL_PSP   (1u << 3)

#define E_TX_EOP     (1u << 0)
#define E_TX_IFCS    (1u << 1)
#define E_TX_RS      (1u << 3)
#define E_DD         (1u << 0)

#define E1000_NRX 16
#define E1000_NTX 8
#define E1000_RX_BUFSZ 2048

struct e1000_rxdesc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_txdesc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

static volatile struct e1000_rxdesc e1000_rxring[E1000_NRX] __attribute__((aligned(16)));
static volatile struct e1000_txdesc e1000_txring[E1000_NTX] __attribute__((aligned(16)));
static uint8_t *e1000_rxbuf[E1000_NRX];
static uint8_t e1000_txbuf[E1000_NTX][E1000_RX_BUFSZ] __attribute__((aligned(16)));
static volatile uint32_t *e1000_mmio;
static uint8_t e1000_mac[ETH_ALEN];
static unsigned e1000_rxcur, e1000_txcur;
static int e1000_present;

static uint32_t e1000_read(unsigned reg) {
    uint32_t v = e1000_mmio[reg / 4];
    __asm__ volatile("" ::: "memory");
    return v;
}
static void e1000_write(unsigned reg, uint32_t v) {
    e1000_mmio[reg / 4] = v;
    __asm__ volatile("" ::: "memory");
}

static int e1000_link_up(void) {
    if (!e1000_present || !e1000_mmio)
        return 0;
    return (e1000_read(E1000_STATUS) & E_STATUS_LU) != 0;
}

static int e1000_send(const void *data, uint64_t len) {
    if (!e1000_present || !e1000_mmio || !data || len == 0 || len > 1514)
        return -1;

    volatile struct e1000_txdesc *d = &e1000_txring[e1000_txcur];
    for (unsigned i = 0; i < 1000000; ++i) {
        if (d->status & E_DD)
            break;
    }
    if (!(d->status & E_DD))
        return -1;

    unsigned n = (unsigned)len;
    if (n < 60)
        n = 60;
    for (unsigned i = 0; i < n; ++i)
        e1000_txbuf[e1000_txcur][i] = (i < len) ? ((const uint8_t *)data)[i] : 0;

    d->addr = (uint64_t)(uintptr_t)e1000_txbuf[e1000_txcur];
    d->length = (uint16_t)n;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->cmd = E_TX_EOP | E_TX_IFCS | E_TX_RS;
    d->status = 0;
    __asm__ volatile("" ::: "memory");
    e1000_write(E1000_TDT, (e1000_txcur + 1) % E1000_NTX);

    for (unsigned i = 0; i < 1000000; ++i) {
        if (d->status & E_DD) {
            e1000_txcur = (e1000_txcur + 1) % E1000_NTX;
            return (int)len;
        }
    }
    return -1;
}

static void e1000_poll(void) {
    if (!e1000_present || !e1000_mmio)
        return;

    for (unsigned guard = 0; guard < E1000_NRX; ++guard) {
        volatile struct e1000_rxdesc *d = &e1000_rxring[e1000_rxcur];
        if (!(d->status & E_DD))
            break;
        uint16_t len = d->length;
        if (len && len <= E1000_RX_BUFSZ)
            net_handle_packet(e1000_rxbuf[e1000_rxcur], len);
        d->status = 0;
        d->length = 0;
        __asm__ volatile("" ::: "memory");

        e1000_write(E1000_RDT, e1000_rxcur);
        e1000_rxcur = (e1000_rxcur + 1) % E1000_NRX;
    }
}

int knx_e1000_probe(const knx_pci_device *device) {
    if (!device || g_netdev.present || e1000_present)
        return 0;
    if (device->vendor_id != 0x8086)
        return 0;
    uint16_t did = device->device_id;
    if (did != 0x100e && did != 0x100d && did != 0x100f &&
        did != 0x1010 && did != 0x1012 && did != 0x1016)
        return 0;

    uint64_t phys = knx_pci_mmio_bar(device, 0);
    if (!phys)
        return 0;
    if (knx_pci_enable_device(device, KNX_PCI_ENABLE_MEMORY | KNX_PCI_ENABLE_BUS_MASTER) != 0)
        return 0;

    e1000_mmio = (volatile uint32_t *)(uintptr_t)phys;

    e1000_write(E1000_IMC, 0xffffffffu);
    (void)e1000_read(E1000_ICR);

    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E_CTRL_RST);
    for (unsigned i = 0; i < 100000; ++i) {
        if (!(e1000_read(E1000_CTRL) & E_CTRL_RST))
            break;
    }
    e1000_write(E1000_IMC, 0xffffffffu);
    (void)e1000_read(E1000_ICR);

    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E_CTRL_SLU | E_CTRL_ASDE);

    uint32_t ral = e1000_read(E1000_RAL);
    uint32_t rah = e1000_read(E1000_RAH);
    e1000_mac[0] = (uint8_t)ral;
    e1000_mac[1] = (uint8_t)(ral >> 8);
    e1000_mac[2] = (uint8_t)(ral >> 16);
    e1000_mac[3] = (uint8_t)(ral >> 24);
    e1000_mac[4] = (uint8_t)rah;
    e1000_mac[5] = (uint8_t)(rah >> 8);
    e1000_write(E1000_RAL, ral);
    e1000_write(E1000_RAH, (rah & 0xffffu) | (1u << 31));
    for (unsigned i = 0; i < 128; ++i)
        e1000_write(E1000_MTA + i * 4, 0);

    mset((void *)e1000_rxring, 0, sizeof(e1000_rxring));
    for (unsigned i = 0; i < E1000_NRX; ++i) {
        void *p = knx_alloc_page();
        if (!p) {
            e1000_mmio = 0;
            return 0;
        }
        e1000_rxbuf[i] = (uint8_t *)p;
        e1000_rxring[i].addr = (uint64_t)(uintptr_t)p;
        e1000_rxring[i].status = 0;
    }
    e1000_write(E1000_RDBAL, (uint32_t)(uintptr_t)e1000_rxring);
    e1000_write(E1000_RDBAH, (uint32_t)((uint64_t)(uintptr_t)e1000_rxring >> 32));
    e1000_write(E1000_RDLEN, E1000_NRX * 16);
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, E1000_NRX - 1);
    e1000_rxcur = 0;
    e1000_write(E1000_RCTL, E_RCTL_EN | E_RCTL_UPE | E_RCTL_MPE |
                             E_RCTL_BAM | E_RCTL_SECRC);

    mset((void *)e1000_txring, 0, sizeof(e1000_txring));
    for (unsigned i = 0; i < E1000_NTX; ++i) {
        e1000_txring[i].addr = (uint64_t)(uintptr_t)e1000_txbuf[i];
        e1000_txring[i].status = E_DD;
    }
    e1000_write(E1000_TDBAL, (uint32_t)(uintptr_t)e1000_txring);
    e1000_write(E1000_TDBAH, (uint32_t)((uint64_t)(uintptr_t)e1000_txring >> 32));
    e1000_write(E1000_TDLEN, E1000_NTX * 16);
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);
    e1000_txcur = 0;
    e1000_write(E1000_TIPG, 0x0060200Au);
    e1000_write(E1000_TCTL, E_TCTL_EN | E_TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));

    e1000_present = 1;
    netdev_register("e1000", e1000_mac, &e1000_ops);
    knx_log("[e1000]", "Initialized");
    return 1;
}

#define MAX_SOCK 16
#define SOCK_RX_CAP 2048
struct knx_socket {
    int in_use;
    int domain, type, protocol;
    uint32_t bound_addr;
    uint16_t bound_port;
    uint8_t rx[SOCK_RX_CAP];
    uint64_t rx_len;
    uint32_t rx_src_ip;
    uint16_t rx_src_port;
    int rx_ready;
};
static struct knx_socket socks[MAX_SOCK];

static uint16_t ping_id, ping_seq;
static volatile int ping_done;

int knx_net_socket(int domain, int type, int protocol) {
    for (int i = 0; i < MAX_SOCK; ++i) {
        if (!socks[i].in_use) {
            mset(&socks[i], 0, sizeof(socks[i]));
            socks[i].in_use = 1;
            socks[i].domain = domain;
            socks[i].type = type;
            socks[i].protocol = protocol;
            return 100 + i;
        }
    }
    return -1;
}

int knx_net_bind(int fd, uint32_t addr, uint16_t port) {
    int idx = fd - 100;
    if (idx < 0 || idx >= MAX_SOCK || !socks[idx].in_use)
        return -9;
    socks[idx].bound_addr = addr;
    socks[idx].bound_port = port;
    return 0;
}

int knx_net_close(int fd) {
    int idx = fd - 100;
    if (idx < 0 || idx >= MAX_SOCK || !socks[idx].in_use)
        return -9;
    mset(&socks[idx], 0, sizeof(socks[idx]));
    return 0;
}

#define ARP_CACHE_SIZE 16
struct arp_entry { uint32_t ip; uint8_t mac[ETH_ALEN]; int valid; };
static struct arp_entry arp_cache[ARP_CACHE_SIZE];

static void arp_cache_put(uint32_t ip, const uint8_t *mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; ++i) {
        if (!arp_cache[i].valid || arp_cache[i].ip == ip) {
            arp_cache[i].ip = ip;
            mcpy(arp_cache[i].mac, mac, ETH_ALEN);
            arp_cache[i].valid = 1;
            return;
        }
    }
}

static int arp_cache_get(uint32_t ip, uint8_t *mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; ++i) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            mcpy(mac, arp_cache[i].mac, ETH_ALEN);
            return 1;
        }
    }
    return 0;
}

static void net_handle_arp(const uint8_t *pkt, uint64_t len) {
    if (len < 28)
        return;

    const uint8_t *p = pkt;
    uint16_t oper = (uint16_t)((p[6] << 8) | p[7]);
    uint8_t sha[ETH_ALEN];
    mcpy(sha, p + 8, ETH_ALEN);

    uint32_t spa = ((uint32_t)p[14] << 24) | ((uint32_t)p[15] << 16) |
                   ((uint32_t)p[16] << 8) | p[17];
    uint32_t tpa = ((uint32_t)p[24] << 24) | ((uint32_t)p[25] << 16) |
                   ((uint32_t)p[26] << 8) | p[27];
    arp_cache_put(spa, sha);

    if (oper == 1 && tpa == htonl32(net_ip)) {

        uint8_t frame[60];
        mset(frame, 0, sizeof(frame));
        mcpy(frame, sha, ETH_ALEN);
        mcpy(frame + 6, g_netdev.mac, ETH_ALEN);
        frame[12] = 0x08; frame[13] = 0x06;

        uint8_t *a = frame + 14;
        a[0] = 0; a[1] = 1;
        a[2] = 0x08; a[3] = 0x00;
        a[4] = 6; a[5] = 4;
        a[6] = 0; a[7] = 2;
        mcpy(a + 8, g_netdev.mac, ETH_ALEN);
        uint32_t srcip = htonl32(net_ip);
        a[14] = (uint8_t)(srcip >> 24); a[15] = (uint8_t)(srcip >> 16);
        a[16] = (uint8_t)(srcip >> 8);  a[17] = (uint8_t)srcip;
        mcpy(a + 18, sha, ETH_ALEN);

        a[24] = (uint8_t)(spa >> 24); a[25] = (uint8_t)(spa >> 16);
        a[26] = (uint8_t)(spa >> 8);  a[27] = (uint8_t)spa;
        net_send_frame(frame, 42);
    }
}

static void net_handle_ip(const uint8_t *pkt, uint64_t len) {
    if (len < 20 || (pkt[0] >> 4) != 4)
        return;
    uint8_t ihl = (uint8_t)((pkt[0] & 0x0F) * 4);
    if (ihl < 20 || ihl > len)
        return;
    uint16_t total = (uint16_t)((pkt[2] << 8) | pkt[3]);
    if (total > len)
        total = (uint16_t)len;
    uint32_t src = ((uint32_t)pkt[12] << 24) | ((uint32_t)pkt[13] << 16) |
                   ((uint32_t)pkt[14] << 8) | pkt[15];
    uint32_t dst = ((uint32_t)pkt[16] << 24) | ((uint32_t)pkt[17] << 16) |
                   ((uint32_t)pkt[18] << 8) | pkt[19];
    if (dst != htonl32(net_ip))
        return;
    uint8_t proto = pkt[9];
    const uint8_t *payload = pkt + ihl;
    uint64_t plen = total - ihl;

    if (proto == IP_PROTO_ICMP) {
        if (plen < 8)
            return;
        uint8_t type = payload[0];
        uint16_t id = (uint16_t)((payload[4] << 8) | payload[5]);
        uint16_t seq = (uint16_t)((payload[6] << 8) | payload[7]);
        if (type == 0) {
            if (id == ping_id && seq == ping_seq)
                ping_done = 1;
        } else if (type == 8) {
            uint8_t dstmac[ETH_ALEN];
            if (!arp_cache_get(src, dstmac))
                mset(dstmac, 0xFF, ETH_ALEN);
            uint8_t frame[1518];
            mset(frame, 0, sizeof(frame));
            mcpy(frame, dstmac, ETH_ALEN);
            mcpy(frame + 6, g_netdev.mac, ETH_ALEN);
            frame[12] = 0x08; frame[13] = 0x00;

            uint8_t *ip = frame + 14;
            uint16_t tiplen = (uint16_t)(20 + plen);
            ip[0] = 0x45; ip[1] = 0;
            ip[2] = (uint8_t)(tiplen >> 8); ip[3] = (uint8_t)tiplen;
            ip[4] = 0; ip[5] = 0; ip[6] = 0; ip[7] = 0;
            ip[8] = 64; ip[9] = IP_PROTO_ICMP;
            ip[10] = 0; ip[11] = 0;
            uint32_t srcn = htonl32(net_ip);

            ip[12] = (uint8_t)(srcn >> 24); ip[13] = (uint8_t)(srcn >> 16);
            ip[14] = (uint8_t)(srcn >> 8);  ip[15] = (uint8_t)srcn;
            ip[16] = (uint8_t)(src >> 24);  ip[17] = (uint8_t)(src >> 16);
            ip[18] = (uint8_t)(src >> 8);   ip[19] = (uint8_t)src;
            uint16_t csum = ip_checksum(ip, 20);
            ip[10] = (uint8_t)(csum >> 8); ip[11] = (uint8_t)csum;

            mcpy(ip + 20, payload, plen);
            ip[20] = 0;
            uint8_t *icmp = ip + 20;
            icmp[2] = 0; icmp[3] = 0;
            uint16_t ics = ip_checksum(icmp, plen);
            icmp[2] = (uint8_t)(ics >> 8); icmp[3] = (uint8_t)ics;
            net_send_frame(frame, 14 + 20 + plen);
        }
    } else if (proto == IP_PROTO_UDP) {
        if (plen < 8)
            return;
        uint16_t dport = (uint16_t)((payload[2] << 8) | payload[3]);
        for (int i = 0; i < MAX_SOCK; ++i) {
            if (socks[i].in_use && socks[i].bound_port == dport) {
                uint16_t ulen = (uint16_t)((payload[4] << 8) | payload[5]);
                uint64_t data_len = (ulen >= 8) ? (uint64_t)(ulen - 8) : (plen - 8);
                if (data_len > SOCK_RX_CAP)
                    data_len = SOCK_RX_CAP;
                mcpy(socks[i].rx, payload + 8, data_len);
                socks[i].rx_len = data_len;
                socks[i].rx_src_ip = src;
                socks[i].rx_src_port = (uint16_t)((payload[0] << 8) | payload[1]);
                socks[i].rx_ready = 1;
                break;
            }
        }
    }
}

static void net_handle_packet(const uint8_t *pkt, uint64_t len) {
    if (len < ETH_HDR_LEN)
        return;
    uint16_t et = (uint16_t)((pkt[12] << 8) | pkt[13]);
    if (et == ETH_TYPE_ARP)
        net_handle_arp(pkt + ETH_HDR_LEN, len - ETH_HDR_LEN);
    else if (et == ETH_TYPE_IP)
        net_handle_ip(pkt + ETH_HDR_LEN, len - ETH_HDR_LEN);
}

void knx_net_init(void) {

}

int knx_net_parse_ip(const char *text, uint32_t *out) {
    if (!text || !out)
        return -1;
    uint32_t parts[4] = {0, 0, 0, 0};
    int idx = 0;
    const char *p = text;
    while (*p && idx < 4) {
        if (*p >= '0' && *p <= '9') {
            parts[idx] = parts[idx] * 10 + (uint32_t)(*p - '0');
            if (parts[idx] > 255)
                return -1;
        } else if (*p == '.') {
            ++idx;
        } else {
            return -1;
        }
        ++p;
    }
    if (idx != 3)
        return -1;

    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 0;
}

static int arp_resolve(uint32_t ip_host) {
    uint32_t ipn = htonl32(ip_host);
    uint8_t mac[ETH_ALEN];
    if (arp_cache_get(ipn, mac))
        return 0;
    if (!g_netdev.present)
        return -1;

    uint8_t frame[60];
    mset(frame, 0, sizeof(frame));
    mset(frame, 0xFF, ETH_ALEN);
    mcpy(frame + 6, g_netdev.mac, ETH_ALEN);
    frame[12] = 0x08; frame[13] = 0x06;

    uint8_t *a = frame + 14;
    a[0] = 0; a[1] = 1;
    a[2] = 0x08; a[3] = 0x00;
    a[4] = 6; a[5] = 4;
    a[6] = 0; a[7] = 1;
    mcpy(a + 8, g_netdev.mac, ETH_ALEN);
    uint32_t srcip = htonl32(net_ip);
    a[14] = (uint8_t)(srcip >> 24); a[15] = (uint8_t)(srcip >> 16);
    a[16] = (uint8_t)(srcip >> 8);  a[17] = (uint8_t)srcip;
    mset(a + 18, 0, ETH_ALEN);
    a[24] = (uint8_t)(ipn >> 24); a[25] = (uint8_t)(ipn >> 16);
    a[26] = (uint8_t)(ipn >> 8);  a[27] = (uint8_t)ipn;
    net_send_frame(frame, 42);

    uint64_t deadline = knx_ticks() + 100;
    while (knx_ticks() < deadline) {
        net_poll();
        if (arp_cache_get(ipn, mac))
            return 0;
    }
    return -1;
}

static uint32_t resolve_dst_mac_host(uint32_t dst_ip_host) {

    uint32_t ipn = htonl32(dst_ip_host);
    uint32_t netipn = htonl32(net_ip);
    uint32_t maskn = htonl32(net_mask);
    uint32_t target = ((ipn & maskn) == (netipn & maskn)) ? dst_ip_host : net_gw;
    arp_resolve(target);
    return target;
}

void knx_net_ping(uint32_t target_hostip) {

    uint32_t target_host = htonl32(target_hostip);
    if (!g_netdev.present) {
        knx_puts("No network interface.\n");
        return;
    }
    uint32_t gw_or_dst = resolve_dst_mac_host(target_host);
    uint32_t gn = htonl32(gw_or_dst);
    uint8_t dstmac[ETH_ALEN];
    if (!arp_cache_get(gn, dstmac)) {
        knx_puts("ARP resolution failed.\n");
        return;
    }

    ping_id = 0x1234;
    ping_seq = 1;
    ping_done = 0;

    uint8_t frame[128];
    mset(frame, 0, sizeof(frame));
    mcpy(frame, dstmac, ETH_ALEN);
    mcpy(frame + 6, g_netdev.mac, ETH_ALEN);
    frame[12] = 0x08; frame[13] = 0x00;

    uint8_t *ip = frame + 14;
    uint16_t tiplen = 20 + 8;
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = (uint8_t)(tiplen >> 8); ip[3] = (uint8_t)tiplen;
    ip[4] = 0; ip[5] = 0; ip[6] = 0; ip[7] = 0;
    ip[8] = 64; ip[9] = IP_PROTO_ICMP;
    ip[10] = 0; ip[11] = 0;
    uint32_t srcn = htonl32(net_ip);
    uint32_t dstn = htonl32(target_host);
    ip[12] = (uint8_t)(srcn >> 24); ip[13] = (uint8_t)(srcn >> 16);
    ip[14] = (uint8_t)(srcn >> 8);  ip[15] = (uint8_t)srcn;
    ip[16] = (uint8_t)(dstn >> 24); ip[17] = (uint8_t)(dstn >> 16);
    ip[18] = (uint8_t)(dstn >> 8);  ip[19] = (uint8_t)dstn;
    uint16_t csum = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(csum >> 8); ip[11] = (uint8_t)csum;

    uint8_t *icmp = ip + 20;
    icmp[0] = 8; icmp[1] = 0;
    icmp[2] = 0; icmp[3] = 0;
    icmp[4] = (uint8_t)(ping_id >> 8); icmp[5] = (uint8_t)ping_id;
    icmp[6] = (uint8_t)(ping_seq >> 8); icmp[7] = (uint8_t)ping_seq;
    uint16_t ics = ip_checksum(icmp, 8);
    icmp[2] = (uint8_t)(ics >> 8); icmp[3] = (uint8_t)ics;

    net_send_frame(frame, 14 + 20 + 8);
    knx_puts("ping: request sent\n");

    uint64_t start = knx_ticks();
    uint64_t deadline = start + 200;
    while (knx_ticks() < deadline && !ping_done)
        net_poll();
    if (ping_done) {
        knx_puts("ping: reply from target in ");
        knx_print_u64(knx_ticks() - start);
        knx_puts(" ticks\n");
    } else {
        knx_puts("ping: no reply (timeout)\n");
    }
}

int knx_net_sendto(int fd, const void *buf, uint64_t len, uint64_t flags,
                   uint32_t addr, uint16_t port) {
    (void)flags;
    int idx = fd - 100;
    if (idx < 0 || idx >= MAX_SOCK || !socks[idx].in_use)
        return -9;
    if (!g_netdev.present)
        return -1;

    uint32_t dst_host = htonl32(addr);
    uint32_t dst_for_arp = resolve_dst_mac_host(dst_host);
    uint32_t dn = htonl32(dst_for_arp);
    uint8_t dstmac[ETH_ALEN];
    if (!arp_cache_get(dn, dstmac))
        return -1;

    uint8_t frame[1518];
    mset(frame, 0, sizeof(frame));
    mcpy(frame, dstmac, ETH_ALEN);
    mcpy(frame + 6, g_netdev.mac, ETH_ALEN);
    frame[12] = 0x08; frame[13] = 0x00;

    uint8_t *ip = frame + 14;
    uint64_t copylen = (len > 1472) ? 1472 : len;
    uint16_t ulen = (uint16_t)(8 + copylen);
    uint16_t tiplen = (uint16_t)(20 + ulen);
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = (uint8_t)(tiplen >> 8); ip[3] = (uint8_t)tiplen;
    ip[4] = 0; ip[5] = 0; ip[6] = 0; ip[7] = 0;
    ip[8] = 64; ip[9] = IP_PROTO_UDP;
    ip[10] = 0; ip[11] = 0;
    uint32_t srcn = htonl32(net_ip);
    uint32_t dstn = htonl32(dst_host);
    ip[12] = (uint8_t)(srcn >> 24); ip[13] = (uint8_t)(srcn >> 16);
    ip[14] = (uint8_t)(srcn >> 8);  ip[15] = (uint8_t)srcn;
    ip[16] = (uint8_t)(dstn >> 24); ip[17] = (uint8_t)(dstn >> 16);
    ip[18] = (uint8_t)(dstn >> 8);  ip[19] = (uint8_t)dstn;
    uint16_t csum = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(csum >> 8); ip[11] = (uint8_t)csum;

    uint8_t *udp = ip + 20;
    uint16_t sport = socks[idx].bound_port ? socks[idx].bound_port : 49152;
    udp[0] = (uint8_t)(sport >> 8); udp[1] = (uint8_t)sport;
    udp[2] = (uint8_t)(port >> 8);  udp[3] = (uint8_t)port;
    udp[4] = (uint8_t)(ulen >> 8);  udp[5] = (uint8_t)ulen;
    udp[6] = 0; udp[7] = 0;
    mcpy(udp + 8, buf, copylen);

    net_send_frame(frame, 14 + 20 + ulen);
    return (int)copylen;
}

int knx_net_recvfrom(int fd, void *buf, uint64_t len, uint64_t flags,
                     uint32_t *addr, uint16_t *port) {
    (void)flags;
    int idx = fd - 100;
    if (idx < 0 || idx >= MAX_SOCK || !socks[idx].in_use)
        return -9;
    net_poll();
    if (!socks[idx].rx_ready)
        return -11;
    uint64_t n = socks[idx].rx_len;
    if (n > len)
        n = len;
    mcpy(buf, socks[idx].rx, n);
    if (addr)
        *addr = htonl32(socks[idx].rx_src_ip);
    if (port)
        *port = socks[idx].rx_src_port;
    socks[idx].rx_ready = 0;
    return (int)n;
}

static void print_ip(uint32_t host_order_ip) {
    uint32_t n = htonl32(host_order_ip);
    knx_print_u64((n >> 24) & 0xFF); knx_putc('.');
    knx_print_u64((n >> 16) & 0xFF); knx_putc('.');
    knx_print_u64((n >> 8) & 0xFF);  knx_putc('.');
    knx_print_u64(n & 0xFF);
}

void knx_net_ifconfig(void) {
    knx_puts("Network interface: ");
    if (!g_netdev.present) {
        knx_puts("none\n");
        return;
    }
    knx_puts(g_netdev.name);
    knx_puts("\n  MAC: ");
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < ETH_ALEN; ++i) {
        knx_putc(hex[g_netdev.mac[i] >> 4]);
        knx_putc(hex[g_netdev.mac[i] & 15]);
        if (i + 1 < ETH_ALEN)
            knx_putc(':');
    }
    knx_puts("\n  IP: ");
    print_ip(net_ip);
    knx_puts("\n  Mask: ");
    print_ip(net_mask);
    knx_puts("\n  GW: ");
    print_ip(net_gw);
    knx_puts("\n  Link: ");
    knx_puts(g_netdev.ops->link_up() ? "up\n" : "down\n");
}

void knx_net_netstat(void) {
    knx_puts("Sockets:\n");
    for (int i = 0; i < MAX_SOCK; ++i) {
        if (!socks[i].in_use)
            continue;
        knx_puts("  fd ");
        knx_print_u64(100 + i);
        knx_puts(" type ");
        knx_print_u64((uint64_t)socks[i].type);
        knx_puts(" bound port ");
        knx_print_u64(socks[i].bound_port);
        knx_puts("\n");
    }
}

void knx_net_info(void) {
    knx_puts("Network: ");
    if (!g_netdev.present)
        knx_puts("no Ethernet controller\n");
    else {
        knx_puts(g_netdev.name);
        knx_puts(" present, link ");
        knx_puts(g_netdev.ops->link_up() ? "up\n" : "down\n");
    }
}
