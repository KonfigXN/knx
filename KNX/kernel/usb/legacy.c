#include "knx.h"

#define LEGACY_MAX_UHCI 4
#define LEGACY_MAX_OHCI 4
#define LEGACY_MAX_EHCI 4
#define LEGACY_MAX_KBD 8
#define LEGACY_TDS 96

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1,%0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0,%1" ::"a"(value), "Nd"(port));
}

static const char hid_plain[128] = {
    [0x04]='a',[0x05]='b',[0x06]='c',[0x07]='d',[0x08]='e',[0x09]='f',[0x0a]='g',
    [0x0b]='h',[0x0c]='i',[0x0d]='j',[0x0e]='k',[0x0f]='l',[0x10]='m',[0x11]='n',
    [0x12]='o',[0x13]='p',[0x14]='q',[0x15]='r',[0x16]='s',[0x17]='t',[0x18]='u',
    [0x19]='v',[0x1a]='w',[0x1b]='x',[0x1c]='y',[0x1d]='z',
    [0x1e]='1',[0x1f]='2',[0x20]='3',[0x21]='4',[0x22]='5',[0x23]='6',[0x24]='7',
    [0x25]='8',[0x26]='9',[0x27]='0',[0x28]='\n',[0x2a]='\b',[0x2b]='\t',
    [0x2c]=' ',[0x2d]='-',[0x2e]='=',[0x2f]='[',[0x30]=']',[0x31]='\\',
    [0x33]=';',[0x34]='\'',[0x35]='`',[0x36]=',',[0x37]='.',[0x38]='/'
};
static const char hid_shifted[128] = {
    [0x04]='A',[0x05]='B',[0x06]='C',[0x07]='D',[0x08]='E',[0x09]='F',[0x0a]='G',
    [0x0b]='H',[0x0c]='I',[0x0d]='J',[0x0e]='K',[0x0f]='L',[0x10]='M',[0x11]='N',
    [0x12]='O',[0x13]='P',[0x14]='Q',[0x15]='R',[0x16]='S',[0x17]='T',[0x18]='U',
    [0x19]='V',[0x1a]='W',[0x1b]='X',[0x1c]='Y',[0x1d]='Z',
    [0x1e]='!',[0x1f]='@',[0x20]='#',[0x21]='$',[0x22]='%',[0x23]='^',[0x24]='&',
    [0x25]='*',[0x26]='(',[0x27]=')',[0x28]='\n',[0x2a]='\b',[0x2b]='\t',
    [0x2c]=' ',[0x2d]='_',[0x2e]='+',[0x2f]='{',[0x30]='}',[0x31]='|',
    [0x33]=':',[0x34]='"',[0x35]='~',[0x36]='<',[0x37]='>',[0x38]='?'
};
static const uint8_t hid_set1_letters[26] = {
    0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
    0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c
};

static uint8_t hid_usage_to_set1(uint8_t u) {
    if (u >= 0x04 && u <= 0x1d) return hid_set1_letters[u - 0x04];
    if (u >= 0x1e && u <= 0x26) return (uint8_t)(0x02 + (u - 0x1e));
    if (u == 0x27) return 0x0b;
    switch (u) {
    case 0x28: return 0x1c; case 0x29: return 0x01; case 0x2a: return 0x0e;
    case 0x2b: return 0x0f; case 0x2c: return 0x39; case 0x2d: return 0x0c;
    case 0x2e: return 0x0d; case 0x2f: return 0x1a; case 0x30: return 0x1b;
    case 0x31: return 0x2b; case 0x33: return 0x27; case 0x34: return 0x28;
    case 0x35: return 0x29; case 0x36: return 0x33; case 0x37: return 0x34;
    case 0x38: return 0x35; case 0x39: return 0x3a;
    case 0x4f: return 0x4d; case 0x50: return 0x4b; case 0x51: return 0x50;
    case 0x52: return 0x48;
    case 0xe0: return 0x1d; case 0xe1: return 0x2a; case 0xe2: return 0x38;
    case 0xe4: return 0x1d; case 0xe5: return 0x36; case 0xe6: return 0x38;
    default:
        if (u >= 0x3a && u <= 0x45) return (uint8_t)(0x3b + (u - 0x3a));
        return 0;
    }
}

static uint8_t legacy_char_ring[128];
static volatile uint8_t legacy_char_head, legacy_char_tail;

static void legacy_char_push(char c) {
    uint8_t next = (uint8_t)((legacy_char_head + 1) & 127);
    if (next == legacy_char_tail)
        return;
    legacy_char_ring[legacy_char_head] = (uint8_t)c;
    legacy_char_head = next;
}

char knx_legacy_usbkbd_read(void) {
    if (legacy_char_tail == legacy_char_head)
        return 0;
    char c = (char)legacy_char_ring[legacy_char_tail];
    legacy_char_tail = (uint8_t)((legacy_char_tail + 1) & 127);
    return c;
}

static int report_contains(const uint8_t *r, uint8_t u) {
    for (unsigned i = 2; i < 8; ++i)
        if (r[i] == u)
            return 1;
    return 0;
}

static int legacy_first_report;

static uint8_t knx_legacy_last_addr, knx_legacy_last_ep, knx_legacy_last_low;
static uint8_t knx_legacy_last_protocol;
static uint16_t knx_legacy_last_mps;

static void legacy_hid_report(const uint8_t *report, uint8_t *prev) {
    uint8_t mods = 0;
    if (report[0] & 0x22) mods |= KNX_KEYMOD_SHIFT;
    if (report[0] & 0x11) mods |= KNX_KEYMOD_CTRL;
    if (report[0] & 0x44) mods |= KNX_KEYMOD_ALT;
    knx_key_mods_set(mods);
    if (!legacy_first_report) {
        legacy_first_report = 1;
        knx_pcspk_beep(600, 90);
    }
    for (unsigned i = 2; i < 8; ++i) {
        uint8_t u = prev[i];
        if (!u || report_contains(report, u)) continue;
        uint8_t code = hid_usage_to_set1(u);
        if (code) knx_key_event_emit(code, 0);
    }
    for (unsigned i = 2; i < 8; ++i) {
        uint8_t u = report[i];
        if (!u || u > 127 || report_contains(prev, u)) continue;
        uint8_t code = hid_usage_to_set1(u);
        if (code) knx_key_event_emit(code, 1);
        char c = (report[0] & 0x22) ? hid_shifted[u] : hid_plain[u];
        if (c) legacy_char_push(c);
    }
    for (unsigned i = 0; i < 8; ++i)
        prev[i] = report[i];
}

static void legacy_hid_mouse_report(const uint8_t *report) {
    int32_t dx = (int32_t)(int8_t)report[1];
    int32_t dy = (int32_t)(int8_t)report[2];
    knx_mouse_event_emit(dx, -dy, (uint8_t)(report[0] & 0x07));
}

struct usb_setup {
    uint8_t bm_request;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed));

static void make_setup(struct usb_setup *s, uint8_t bm, uint8_t req,
                       uint16_t value, uint16_t index, uint16_t length) {
    s->bm_request = bm; s->request = req; s->value = value;
    s->index = index; s->length = length;
}

static void legacy_delay_ms(uint32_t ms) {
    uint64_t start = knx_ticks();
    uint64_t ticks_needed = (ms + 9) / 10;
    while (knx_ticks() - start < ticks_needed)
        __asm__ volatile("pause");
}

struct hid_iface {
    uint8_t found;
    uint8_t config;
    uint8_t interface;
    uint8_t protocol;
    uint8_t ep;
    uint16_t mps;
};

static void parse_config(const uint8_t *buf, uint32_t len, struct hid_iface *out) {
    unsigned pos = 0;
    const uint8_t *iface = 0, *endpoint = 0;
    uint8_t current_iface = 0;
    while (pos + 2 <= len) {
        uint8_t l = buf[pos], type = buf[pos + 1];
        if (!l || pos + l > len)
            break;
        if (type == 4 && l >= 9) {
            iface = buf + pos;
            current_iface = iface[2];
            endpoint = 0;
        } else if (type == 5 && l >= 7 && iface) {
            uint8_t addr = buf[pos + 2];
            if ((buf[pos + 3] & 0x03) == 3 && (addr & 0x80) && !endpoint) {
                endpoint = buf + pos;

                if (iface[5] == 3 && (iface[7] == 1 || iface[7] == 2)) {
                    out->found = 1;
                    out->interface = current_iface;
                    out->protocol = iface[7];
                    out->ep = (uint8_t)(addr & 0x0f);
                    out->mps = (uint16_t)(buf[pos + 4] & 0x7ff);
                    return;
                }
            }
        }
        pos += l;
    }
}

#define UHCI_TD_TERMINATE 1u
#define UHCI_TD_QH 2u
#define UHCI_TD_DEPTH 4u
#define UHCI_TD_ACTIVE (1u << 23)
#define UHCI_TD_IOC (1u << 24)
#define UHCI_TD_LOW (1u << 26)
#define UHCI_TD_ERROR_SHIFT 27
#define UHCI_TD_PID_SETUP 0x2d
#define UHCI_TD_PID_OUT 0xe1
#define UHCI_TD_PID_IN 0x69

#define UHCI_PORT_CCS 0x0001u
#define UHCI_PORT_CSC 0x0002u
#define UHCI_PORT_PED 0x0004u
#define UHCI_PORT_PEDC 0x0008u
#define UHCI_PORT_RESET 0x0200u
#define UHCI_PORT_SUSPEND 0x1000u
#define UHCI_PORT_LSDA 0x0100u

struct uhci_td {
    volatile uint32_t link, status, token, buffer;
    uint32_t sw[4];
};
struct uhci_qh {
    volatile uint32_t link, element;
    uint32_t sw[6];
};
struct uhci_ctrl {
    uint16_t io;
    uint8_t used;
    uint32_t *frame_list;
    struct uhci_qh *control_qh, *int_qh;
    struct uhci_td *tds;
    uint32_t td_next;
};
static struct uhci_ctrl uhci_ctrls[LEGACY_MAX_UHCI];
static unsigned uhci_count;

static inline uint16_t uhci_in(struct uhci_ctrl *h, unsigned reg) { return inw(h->io + reg); }
static inline void uhci_out(struct uhci_ctrl *h, unsigned reg, uint16_t v) { outw(h->io + reg, v); }

static struct uhci_td *uhci_td_alloc(struct uhci_ctrl *h) {
    if (h->td_next >= LEGACY_TDS)
        return 0;
    struct uhci_td *td = &h->tds[h->td_next++];
    td->link = UHCI_TD_TERMINATE;
    td->status = 0;
    td->token = 0;
    td->buffer = 0;
    return td;
}

static void uhci_init_td(struct uhci_td *td, struct uhci_td *next, uint8_t low,
                         uint8_t addr, uint8_t ep, uint8_t toggle, uint8_t pid,
                         uint16_t len, void *buffer) {
    td->link = next ? ((uint32_t)(uintptr_t)next | UHCI_TD_DEPTH)
                    : UHCI_TD_TERMINATE;
    td->status = (3u << UHCI_TD_ERROR_SHIFT) | UHCI_TD_ACTIVE |
                 (low ? UHCI_TD_LOW : 0);
    td->token = (uint32_t)(((len ? len - 1 : 0) & 0x7ff) << 21) |
                (uint32_t)(toggle ? 1u << 19 : 0) |
                (uint32_t)(ep << 15) | (uint32_t)(addr << 8) | pid;
    td->buffer = (uint32_t)(uintptr_t)buffer;
}

static int uhci_wait_td(struct uhci_td *td) {
    uint64_t deadline = knx_ticks() + 30;
    while ((td->status & UHCI_TD_ACTIVE) && knx_ticks() < deadline)
        __asm__ volatile("pause");
    if (td->status & UHCI_TD_ACTIVE)
        return -1;

    if (td->status & ((1u << 22) | (1u << 21) | (1u << 20) |
                      (1u << 18) | (1u << 17)))
        return -1;
    return 0;
}

static int uhci_control(struct uhci_ctrl *h, uint8_t addr, uint8_t low,
                        uint8_t bm, uint8_t req, uint16_t value, uint16_t index,
                        uint16_t length, void *buffer) {
    struct usb_setup setup;
    make_setup(&setup, bm, req, value, index, length);
    struct uhci_td *t1 = uhci_td_alloc(h), *t2 = 0, *t3 = uhci_td_alloc(h);
    if (!t1 || !t3)
        return -1;
    uint8_t out = !(bm & 0x80);
    if (length) {
        t2 = uhci_td_alloc(h);
        if (!t2) return -1;
    }
    uhci_init_td(t1, t2 ? t2 : t3, low, addr, 0, 0, UHCI_TD_PID_SETUP, 8, &setup);
    if (t2)
        uhci_init_td(t2, t3, low, addr, 0, 1,
                     out ? UHCI_TD_PID_OUT : UHCI_TD_PID_IN, length, buffer);
    uhci_init_td(t3, 0, low, addr, 0, 1,
                 out ? UHCI_TD_PID_IN : UHCI_TD_PID_OUT, 0, 0);
    t3->status |= UHCI_TD_IOC;
    h->control_qh->element = (uint32_t)(uintptr_t)t1;
    return uhci_wait_td(t3) ? -1 : 0;
}

static int uhci_port_reset(struct uhci_ctrl *h, unsigned port, uint8_t *low) {
    unsigned reg = 0x10 + port * 2;
    uint16_t sc = uhci_in(h, reg);
    if (!(sc & UHCI_PORT_CCS))
        return 0;
    uhci_out(h, reg, (uint16_t)((sc & ~UHCI_PORT_SUSPEND) | UHCI_PORT_RESET));
    legacy_delay_ms(50);
    sc = uhci_in(h, reg);
    uhci_out(h, reg, (uint16_t)((sc & ~(UHCI_PORT_RESET | UHCI_PORT_SUSPEND)) |
                                UHCI_PORT_CSC));
    legacy_delay_ms(5);
    uhci_out(h, reg, (uint16_t)(UHCI_PORT_PED | UHCI_PORT_CSC));
    legacy_delay_ms(5);
    uint16_t final_sc = uhci_in(h, reg);
    *low = (uint8_t)((final_sc & UHCI_PORT_LSDA) != 0);
    return (final_sc & (UHCI_PORT_CCS | UHCI_PORT_PED)) ==
           (UHCI_PORT_CCS | UHCI_PORT_PED);
}

static int uhci_enumerate(struct uhci_ctrl *h, unsigned port) {
    uint8_t low;
    if (!uhci_port_reset(h, port, &low))
        return 0;
    uint8_t desc[18];
    for (unsigned i = 0; i < sizeof(desc); ++i) desc[i] = 0;
    if (uhci_control(h, 0, low, 0x80, 6, 0x0100, 0, 8, desc))
        return 0;
    uint16_t desc_len = (uint16_t)desc[0];
    if (desc_len < 8 || desc_len > 18)
        desc_len = 18;
    if (uhci_control(h, 0, low, 0x80, 6, 0x0100, 0, desc_len, desc))
        return 0;
    if (desc_len < 18)
        return 0;
    static uint8_t next_addr = 1;
    uint8_t addr = next_addr++;
    if (uhci_control(h, 0, low, 0x00, 5, addr, 0, 0, 0))
        return 0;
    legacy_delay_ms(3);
    uint8_t cfg[255];
    for (unsigned i = 0; i < sizeof(cfg); ++i) cfg[i] = 0;
    if (uhci_control(h, addr, low, 0x80, 6, 0x0200, 0, 9, cfg))
        return 0;
    uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8));
    if (total < 9 || total > sizeof(cfg))
        total = 9;
    if (uhci_control(h, addr, low, 0x80, 6, 0x0200, 0, total, cfg))
        return 0;
    struct hid_iface hid;
    hid.found = 0;
    hid.protocol = 1;
    parse_config(cfg, total, &hid);
    if (!hid.found)
        return 0;
    knx_legacy_last_addr = addr;
    knx_legacy_last_ep = hid.ep;
    knx_legacy_last_low = low;
    knx_legacy_last_protocol = hid.protocol;
    knx_legacy_last_mps = hid.mps;
    if (uhci_control(h, addr, low, 0x00, 9, hid.config, 0, 0, 0))
        return 0;
    legacy_delay_ms(3);

    if (uhci_control(h, addr, low, 0x21, 0x0b, 1, hid.interface, 0, 0))
        return 0;
    uhci_control(h, addr, low, 0x21, 0x0a, 0, hid.interface, 0, 0);
    return 1;
}

#define OHCI_CTRL_PLE (1u << 2)
#define OHCI_CTRL_CLE (1u << 4)
#define OHCI_CTRL_HCFS_OPER (2u << 6)
#define OHCI_TD_R (1u << 18)
#define OHCI_TD_DP_IN (2u << 19)
#define OHCI_TD_DP_OUT (1u << 19)
#define OHCI_TD_DP_SETUP (0u << 19)
#define OHCI_TD_DI_NOINT (7u << 21)
#define OHCI_TD_EC (3u << 26)

struct ohci_td {
    volatile uint32_t ctrl, cbp, next, be;
    uint32_t sw[4];
};
struct ohci_ed {
    volatile uint32_t ctrl, tail, head, next;
};
struct ohci_hcca {
    volatile uint32_t intr[32];
    volatile uint16_t frame, pad;
    volatile uint32_t done_head;
    uint8_t reserved[116];
};
struct ohci_ctrl {
    volatile uint8_t *base;
    uint8_t used;
    struct ohci_hcca *hcca;
    struct ohci_ed *control_ed, *int_ed;
    struct ohci_td *tds;
    uint32_t td_next;
};
static struct ohci_ctrl ohci_ctrls[LEGACY_MAX_OHCI];
static unsigned ohci_count;

static inline uint32_t ohci_read(struct ohci_ctrl *h, unsigned reg) {
    return *(volatile uint32_t *)(h->base + reg);
}
static inline void ohci_write(struct ohci_ctrl *h, unsigned reg, uint32_t v) {
    *(volatile uint32_t *)(h->base + reg) = v;
}

static struct ohci_td *ohci_td_alloc(struct ohci_ctrl *h) {
    if (h->td_next >= LEGACY_TDS)
        return 0;
    struct ohci_td *td = &h->tds[h->td_next++];
    td->ctrl = OHCI_TD_R | OHCI_TD_DI_NOINT | OHCI_TD_EC;
    td->cbp = td->next = td->be = 0;
    return td;
}

static void ohci_set_ed(struct ohci_ed *ed, struct ohci_td *first,
                        struct ohci_td *last) {
    ed->tail = (uint32_t)(uintptr_t)last;
    ed->head = (uint32_t)(uintptr_t)first;
}

static int ohci_wait_ed(struct ohci_ed *ed) {
    uint64_t deadline = knx_ticks() + 10;
    while (((ed->head & ~0xfu) != ed->tail) && knx_ticks() < deadline)
        __asm__ volatile("pause");
    if ((ed->head & ~0xfu) != ed->tail)
        return -1;
    return 0;
}

static int ohci_control(struct ohci_ctrl *h, uint8_t addr, uint8_t low,
                        uint8_t bm, uint8_t req, uint16_t value, uint16_t index,
                        uint16_t length, void *buffer) {
    struct usb_setup setup;
    make_setup(&setup, bm, req, value, index, length);
    struct ohci_td *t1 = ohci_td_alloc(h), *t2 = 0, *t3 = ohci_td_alloc(h);
    struct ohci_td *sentinel = ohci_td_alloc(h);
    if (!t1 || !t3 || !sentinel) return -1;
    uint8_t out = !(bm & 0x80);
    if (length) {
        t2 = ohci_td_alloc(h);
        if (!t2) return -1;
    }
    sentinel->ctrl = sentinel->cbp = sentinel->next = sentinel->be = 0;
    t1->ctrl |= OHCI_TD_DP_SETUP | (0u << 24);
    t1->cbp = (uint32_t)(uintptr_t)&setup;
    t1->be = t1->cbp + 7;
    t1->next = (uint32_t)(uintptr_t)(t2 ? t2 : t3);
    if (t2) {
        t2->ctrl |= (out ? OHCI_TD_DP_OUT : OHCI_TD_DP_IN) | (1u << 25);
        t2->cbp = (uint32_t)(uintptr_t)buffer;
        t2->be = t2->cbp + length - 1;
        t2->next = (uint32_t)(uintptr_t)t3;
    }
    t3->ctrl |= (out ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT) | (1u << 25);
    t3->cbp = t3->be = 0;
    t3->next = (uint32_t)(uintptr_t)sentinel;
    h->control_ed->ctrl = (uint32_t)addr | (low ? (1u << 13) : 0) | (8u << 16);
    ohci_set_ed(h->control_ed, t1, sentinel);
    ohci_write(h, 0x08, 1u << 1);
    if (ohci_wait_ed(h->control_ed))
        return -1;
    if ((t1->ctrl >> 28) || (t2 && (t2->ctrl >> 28)) || (t3->ctrl >> 28))
        return -1;
    return 0;
}

static int ohci_port_reset(struct ohci_ctrl *h, unsigned port, uint8_t *low) {
    unsigned reg = 0x54 + port * 4;
    uint32_t status = ohci_read(h, reg);
    if (!(status & 1))
        return 0;
    ohci_write(h, reg, 1u << 4);
    legacy_delay_ms(50);
    uint64_t deadline = knx_ticks() + 10;
    while ((ohci_read(h, reg) & (1u << 4)) && knx_ticks() < deadline)
        __asm__ volatile("pause");
    status = ohci_read(h, reg);
    ohci_write(h, reg, (status & ~(1u << 4)) | 0x0au);
    status = ohci_read(h, reg);
    *low = (uint8_t)((status >> 7) & 1);
    return (status & 3) == 3;
}

static int ohci_enumerate(struct ohci_ctrl *h, unsigned port) {
    uint8_t low;
    if (!ohci_port_reset(h, port, &low))
        return 0;
    uint8_t desc[18];
    for (unsigned i = 0; i < sizeof(desc); ++i) desc[i] = 0;
    if (ohci_control(h, 0, low, 0x80, 6, 0x0100, 0, 8, desc))
        return 0;
    uint16_t desc_len = (uint16_t)desc[0];
    if (desc_len < 8 || desc_len > 18)
        desc_len = 18;
    if (ohci_control(h, 0, low, 0x80, 6, 0x0100, 0, desc_len, desc))
        return 0;
    if (desc_len < 18) return 0;
    static uint8_t next_addr = 1;
    uint8_t addr = next_addr++;
    if (ohci_control(h, 0, low, 0x00, 5, addr, 0, 0, 0))
        return 0;
    legacy_delay_ms(3);
    uint8_t cfg[255];
    for (unsigned i = 0; i < sizeof(cfg); ++i) cfg[i] = 0;
    if (ohci_control(h, addr, low, 0x80, 6, 0x0200, 0, 9, cfg))
        return 0;
    uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8));
    if (total < 9 || total > sizeof(cfg)) total = 9;
    if (ohci_control(h, addr, low, 0x80, 6, 0x0200, 0, total, cfg))
        return 0;
    struct hid_iface hid;
    hid.found = 0;
    hid.protocol = 1;
    parse_config(cfg, total, &hid);
    if (!hid.found)
        return 0;
    knx_legacy_last_addr = addr;
    knx_legacy_last_ep = hid.ep;
    knx_legacy_last_low = low;
    knx_legacy_last_protocol = hid.protocol;
    knx_legacy_last_mps = hid.mps;
    if (ohci_control(h, addr, low, 0x00, 9, hid.config, 0, 0, 0)) return 0;
    legacy_delay_ms(3);
    if (ohci_control(h, addr, low, 0x21, 0x0b, 1, hid.interface, 0, 0)) return 0;
    ohci_control(h, addr, low, 0x21, 0x0a, 0, hid.interface, 0, 0);
    return 1;
}

struct legacy_kbd {
    uint8_t used, type, ctrl, addr, ep, low, toggle, mps, protocol;
    uint8_t prev[8];
    void *report_td;
    uint8_t *report_buf;
};
static struct legacy_kbd keyboards[LEGACY_MAX_KBD];
static unsigned keyboard_count;

static struct legacy_kbd *keyboard_alloc(void) {
    if (keyboard_count >= LEGACY_MAX_KBD) return 0;
    struct legacy_kbd *k = &keyboards[keyboard_count++];
    for (unsigned i = 0; i < 8; ++i) k->prev[i] = 0;
    return k;
}

static void arm_uhci_keyboard(struct uhci_ctrl *h, struct legacy_kbd *k) {
    struct uhci_td *td = uhci_td_alloc(h);
    if (!td) return;
    uhci_init_td(td, 0, k->low, k->addr, k->ep, k->toggle, UHCI_TD_PID_IN,
                 8, k->report_buf);
    h->int_qh->element = (uint32_t)(uintptr_t)td;
    k->report_td = td;
}

static void arm_ohci_keyboard(struct ohci_ctrl *h, struct legacy_kbd *k) {
    struct ohci_td *td = ohci_td_alloc(h);
    if (!td) return;
    td->ctrl |= OHCI_TD_DP_IN | (uint32_t)(k->toggle ? 1u << 25 : 0);
    td->cbp = (uint32_t)(uintptr_t)k->report_buf;
    td->be = td->cbp + 7;
    td->next = 0;
    ohci_set_ed(h->int_ed, td, td);
    k->report_td = td;
}

static void legacy_register_device(uint8_t type, uint8_t ctrl, uint8_t addr,
                                   uint8_t ep, uint8_t low, uint8_t mps,
                                   uint8_t protocol, uint8_t *report_buf) {
    struct legacy_kbd *k = keyboard_alloc();
    if (!k) return;
    k->used = 1; k->type = type; k->ctrl = ctrl; k->addr = addr;
    k->ep = ep; k->low = low; k->toggle = 0; k->mps = mps;
    k->protocol = protocol;
    k->report_buf = report_buf;
    if (type == 0)
        arm_uhci_keyboard(&uhci_ctrls[ctrl], k);
    else
        arm_ohci_keyboard(&ohci_ctrls[ctrl], k);
    knx_puts(protocol == 2 ? "[USB-MOUSE] Boot mouse bound on legacy host controller\n"
                           : "[USB-KBD] Boot keyboard bound on legacy host controller\n");
}

void knx_legacy_usb_tick(void) {
    for (unsigned i = 0; i < keyboard_count; ++i) {
        struct legacy_kbd *k = &keyboards[i];
        if (!k->used || !k->report_td) continue;
        int done = 0;
        if (k->type == 0) {
            struct uhci_td *td = (struct uhci_td *)k->report_td;
            if (!(td->status & UHCI_TD_ACTIVE)) {
                done = 1;
                if (td->status & ((1u << 22) | (1u << 21) | (1u << 20) |
                                  (1u << 19) | (1u << 18) | (1u << 17)))
                    done = 0;
            }
        } else {
            struct ohci_ed *ed = ohci_ctrls[k->ctrl].int_ed;
            if ((ed->head & ~0xfu) == ed->tail)
                done = 1;
        }
        if (!done) continue;
        if (k->protocol == 2)
            legacy_hid_mouse_report(k->report_buf);
        else
            legacy_hid_report(k->report_buf, k->prev);
        k->toggle ^= 1;
        if (k->type == 0)
            arm_uhci_keyboard(&uhci_ctrls[k->ctrl], k);
        else
            arm_ohci_keyboard(&ohci_ctrls[k->ctrl], k);
    }
}

int knx_uhci_probe(const knx_pci_device *device) {
    if (!device || uhci_count >= LEGACY_MAX_UHCI) return 0;
    uint16_t io = (uint16_t)(device->bar[4] & 0xfffffffcu);
    if (!io) return 0;
    if (knx_pci_enable_device(device, KNX_PCI_ENABLE_IO | KNX_PCI_ENABLE_BUS_MASTER))
        return 0;
    struct uhci_ctrl *h = &uhci_ctrls[uhci_count];
    h->io = io;
    h->used = 1;
    h->frame_list = knx_alloc_page();
    h->control_qh = knx_alloc_page();
    h->int_qh = (struct uhci_qh *)((uint8_t *)h->control_qh + 64);
    h->tds = knx_alloc_page();
    if (!h->frame_list || !h->control_qh || !h->tds) {
        h->used = 0;
        return 0;
    }
    for (unsigned i = 0; i < 1024; ++i)
        h->frame_list[i] = (uint32_t)(uintptr_t)h->control_qh | UHCI_TD_QH;
    h->control_qh->link = (uint32_t)(uintptr_t)h->int_qh | UHCI_TD_QH;
    h->control_qh->element = UHCI_TD_TERMINATE;
    h->int_qh->link = UHCI_TD_TERMINATE;
    h->int_qh->element = UHCI_TD_TERMINATE;
    uhci_out(h, 0x00, 0);
    uhci_out(h, 0x02, 0xffff);
    uhci_out(h, 0x04, 0);
    uint32_t frame_phys = (uint32_t)(uintptr_t)h->frame_list;
    uhci_out(h, 0x08, (uint16_t)frame_phys);
    uhci_out(h, 0x0a, (uint16_t)(frame_phys >> 16));
    uhci_out(h, 0x0c, 0x40);
    uhci_out(h, 0x00, 0x01);
    unsigned idx = uhci_count++;
    for (unsigned p = 0; p < 2; ++p) {
        uint8_t *buf = knx_alloc_page();
        if (!buf) break;
        for (unsigned i = 0; i < 8; ++i) buf[i] = 0;

        if (!uhci_enumerate(h, p)) continue;
        legacy_register_device(0, (uint8_t)idx, knx_legacy_last_addr,
                               knx_legacy_last_ep, knx_legacy_last_low,
                               knx_legacy_last_mps, knx_legacy_last_protocol, buf);
    }
    knx_puts("[UHCI] Host controller initialized\n");
    return 1;
}

int knx_ohci_probe(const knx_pci_device *device) {
    if (!device || ohci_count >= LEGACY_MAX_OHCI) return 0;
    if (device->prog_if != 0x01 && device->prog_if != 0x10)
        return 0;
    uint64_t phys = knx_pci_mmio_bar(device, 0);
    if (!phys) return 0;
    if (knx_vm_map_mmio(phys, 0x1000)) return 0;
    if (knx_pci_enable_device(device, KNX_PCI_ENABLE_MEMORY | KNX_PCI_ENABLE_BUS_MASTER))
        return 0;
    struct ohci_ctrl *h = &ohci_ctrls[ohci_count];
    h->base = (volatile uint8_t *)(uintptr_t)phys;
    h->used = 1;
    ohci_write(h, 0x08, 1u);
    {
        uint64_t reset_deadline = knx_ticks() + 5;
        while ((ohci_read(h, 0x08) & 1u) && knx_ticks() < reset_deadline)
            __asm__ volatile("pause");
    }
    h->hcca = knx_alloc_page();
    h->control_ed = knx_alloc_page();
    h->int_ed = (struct ohci_ed *)((uint8_t *)h->control_ed + 64);
    h->tds = knx_alloc_page();
    if (!h->hcca || !h->control_ed || !h->tds) {
        h->used = 0;
        return 0;
    }
    for (unsigned i = 0; i < 32; ++i)
        h->hcca->intr[i] = (uint32_t)(uintptr_t)h->int_ed;
    h->hcca->done_head = 0;
    h->control_ed->ctrl = 8u << 16;
    h->control_ed->tail = h->control_ed->head = 0;
    h->control_ed->next = 0;
    h->int_ed->ctrl = (2u << 11) | (8u << 16);
    h->int_ed->tail = h->int_ed->head = 0;
    h->int_ed->next = 0;
    ohci_write(h, 0x0c, 0xffffffff);
    ohci_write(h, 0x10, 0);
    ohci_write(h, 0x18, (uint32_t)(uintptr_t)h->hcca);
    ohci_write(h, 0x20, (uint32_t)(uintptr_t)h->control_ed);
    ohci_write(h, 0x24, (uint32_t)(uintptr_t)h->control_ed);
    ohci_write(h, 0x28, 0);
    ohci_write(h, 0x2c, 0);
    if (!ohci_read(h, 0x34))
        ohci_write(h, 0x34, 0x2edf);
    ohci_write(h, 0x40, 0x2a30);
    ohci_write(h, 0x50, 1u << 3);
    ohci_write(h, 0x04, OHCI_CTRL_PLE | OHCI_CTRL_CLE | OHCI_CTRL_HCFS_OPER);
    legacy_delay_ms(10);
    unsigned idx = ohci_count++;
    uint32_t n_ports = ohci_read(h, 0x48) & 0xff;
    if (n_ports > 15) n_ports = 15;
    for (unsigned p = 0; p < n_ports; ++p) {
        uint8_t *buf = knx_alloc_page();
        if (!buf) break;
        for (unsigned i = 0; i < 8; ++i) buf[i] = 0;
        if (!ohci_enumerate(h, p)) continue;
        legacy_register_device(1, (uint8_t)idx, knx_legacy_last_addr,
                               knx_legacy_last_ep, knx_legacy_last_low,
                               knx_legacy_last_mps, knx_legacy_last_protocol, buf);
    }
    knx_puts("[OHCI] Host controller initialized\n");
    return 1;
}

int knx_ehci_probe(const knx_pci_device *device) {
    if (!device) return 0;
    uint64_t phys = knx_pci_mmio_bar(device, 0);
    if (!phys) return 0;
    if (knx_vm_map_mmio(phys, 0x1000)) return 0;
    if (knx_pci_enable_device(device, KNX_PCI_ENABLE_MEMORY | KNX_PCI_ENABLE_BUS_MASTER))
        return 0;
    volatile uint8_t *base = (volatile uint8_t *)(uintptr_t)phys;
    uint32_t cap = *(volatile uint32_t *)base;
    uint32_t caplen = cap & 0xff;
    uint32_t op = caplen;
    uint32_t hcsparams = *(volatile uint32_t *)(base + 4);
    uint32_t ports = hcsparams & 0xff;
    if (ports > 64) ports = 64;
    if (caplen < 0x20 || caplen > 0x80)
        return 0;

    *(volatile uint32_t *)(base + op + 0x40) = 0;
    for (unsigned p = 0; p < ports; ++p) {
        volatile uint32_t *sc = (volatile uint32_t *)(base + op + 0x44 + p * 4);
        uint32_t v = *sc;
        v &= ~(1u << 2);
        v |= 1u << 13;
        *sc = v;
    }
    knx_puts("[EHCI] Root ports released to USB 1.1 companions\n");
    return 1;
}
