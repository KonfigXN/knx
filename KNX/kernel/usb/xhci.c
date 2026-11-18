#include "knx.h"

#define MAX_XHCI 4
#define MAX_BINDINGS 16
#define MAX_IFACES 4
#define XHCI_STS_HCHALTED (1u << 0)
#define XHCI_PORT_CCS (1u << 0)
#define XHCI_PORT_PED (1u << 1)
#define XHCI_PORT_PR (1u << 4)
#define XHCI_PORT_PP (1u << 9)
#define XHCI_PORT_PS_MASK 0xfu
#define XHCI_PORT_CLEAR 0x00fe0000u

#define USBCMD_RS (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH (1u << 0)

#define TRB_CYCLE (1u << 0)
#define TRB_TC (1u << 1)
#define TRB_ISP (1u << 2)
#define TRB_CH (1u << 4)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_BSR (1u << 9)
#define TRB_DIR (1u << 16)
#define TRB_TYPE_SHIFT 10
#define TRB_TYPE(t) ((t) << TRB_TYPE_SHIFT)

#define TR_NORMAL 1
#define TR_SETUP 2
#define TR_DATA 3
#define TR_STATUS 4
#define TR_LINK 6
#define TR_ENABLE_SLOT 9
#define TR_ADDRESS_DEVICE 11
#define TR_CONFIGURE_ENDPOINT 12

#define EV_TRANSFER 32
#define EV_COMMAND_COMPLETE 33
#define EV_PORT_STATUS_CHANGE 34

#define CC_SUCCESS 1

#define ERDP_EHB (1u << 3)

#define RING_TRBS 256u
#define EVENT_TRBS 256u

#define HID_BATCH 16u
#define HID_REPORT_SLOT 16u
#define TRB_BUF_NONE 0xffu

struct trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
};

struct ring {
    struct trb *trbs;
    uint64_t phys;
    uint32_t size;
    uint32_t enq;
    uint32_t pcs;
};

struct hid_binding {
    uint8_t in_use;
    uint8_t ready;
    uint8_t controller;
    uint8_t slot;
    uint8_t ep_id;
    uint8_t ep_num;
    uint16_t ep_mps;
    uint8_t protocol;
    uint8_t interface_number;
    struct ring ring;
    uint8_t *reports;
    uint8_t *report_slot[HID_BATCH];
    uint8_t *report;
    uint8_t trb_buf[RING_TRBS];
    uint32_t deq;
    uint8_t prev_report[8];
    uint8_t char_ring[64];
    uint8_t char_head, char_tail;
    uint8_t first_key_beep;
};

struct xhci_controller {
    volatile uint8_t *base;
    uint8_t index;
    uint8_t bus, dev, fn;
    uint32_t op, rt, db;
    uint8_t ports;
    uint8_t max_slots;
    uint8_t csz;
    uint32_t stride;
    uint16_t version;
    uint8_t halted;
    uint8_t started;
    uint8_t slot;
    uint8_t probe_no_hid;
    uint8_t ppc;
    uint8_t pp_reported[64];
    uint8_t port_scanned[64];
    uint8_t port_cooldown[64];
    uint8_t port_ccs[64];
    struct ring cmd, ep0;
    struct trb *ev_ring;
    uint64_t ev_phys;
    uint32_t ev_size, ev_idx, ev_ccs;
    uint8_t *erst;
    uint8_t *dcbaap;
    uint8_t *ictx;
    uint8_t *setup;
    uint8_t *xfer;
    uint8_t cmd_done, cmd_code, cmd_slot;
    uint8_t xfer_done, xfer_code;
    uint32_t xfer_len;
    uint32_t erdp_hi;
};

static struct xhci_controller controllers[MAX_XHCI];
static unsigned controller_count;
static struct hid_binding bindings[MAX_BINDINGS];

extern void knx_mouse_event_emit(int32_t dx, int32_t dy, uint8_t buttons);

static void hid_kbd_report(struct hid_binding *b);
static void hid_mouse_report(struct hid_binding *b);
static void hid_complete(struct hid_binding *b, uint64_t param);

static uint32_t mmio32(volatile uint8_t *p, uint32_t offset) {
    return *(volatile uint32_t *)(p + offset);
}

static void mmio_write32(volatile uint8_t *p, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(p + offset) = value;
}

static void mmio_write64(volatile uint8_t *p, uint32_t offset, uint64_t value) {
    mmio_write32(p, offset, (uint32_t)value);
    mmio_write32(p, offset + 4, (uint32_t)(value >> 32));
}

static uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void *alloc_page(void) {
    uint8_t *page = (uint8_t *)knx_alloc_page();
    if (!page)
        return 0;
    for (unsigned i = 0; i < 4096; ++i)
        page[i] = 0;
    return page;
}

static void *alloc_ring_page(struct ring *ring) {
    uint8_t *page = (uint8_t *)alloc_page();
    if (!page)
        return 0;
    ring->trbs = (struct trb *)page;
    ring->phys = (uint64_t)(uintptr_t)page;
    ring->size = RING_TRBS;
    ring->enq = 0;
    ring->pcs = 1;
    return page;
}

static uint32_t ring_push(struct ring *ring, uint64_t param, uint32_t status,
                          uint32_t control) {
    if (ring->enq == ring->size - 1) {
        struct trb *link = &ring->trbs[ring->size - 1];
        link->param = ring->phys;
        link->status = 0;
        link->control = TRB_TYPE(TR_LINK) | TRB_TC | ring->pcs;
        __asm__ volatile("" ::: "memory");
        ring->pcs ^= 1;
        ring->enq = 0;
    }
    uint32_t index = ring->enq++;
    struct trb *trb = &ring->trbs[index];
    trb->param = param;
    trb->status = status;
    __asm__ volatile("" ::: "memory");
    trb->control = control | ring->pcs;
    __asm__ volatile("" ::: "memory");
    return index;
}

static void ring_reset(struct ring *ring) {
    for (uint32_t i = 0; i < ring->size; ++i) {
        ring->trbs[i].param = 0;
        ring->trbs[i].status = 0;
        ring->trbs[i].control = 0;
    }
    __asm__ volatile("" ::: "memory");
    ring->enq = 0;
    ring->pcs = 1;
}

static void doorbell(struct xhci_controller *h, unsigned slot, unsigned value) {
    mmio_write32(h->base, h->db + slot * 4, value);
}

static struct hid_binding *find_binding(unsigned controller, uint32_t slot,
                                        uint32_t epid) {
    for (unsigned i = 0; i < MAX_BINDINGS; ++i) {
        struct hid_binding *b = &bindings[i];
        if (b->in_use && b->ready && b->controller == controller &&
            b->slot == slot && b->ep_id == epid)
            return b;
    }
    return 0;
}

static void hid_push_trb(struct hid_binding *b, uint8_t bi) {
    uint32_t index = ring_push(&b->ring, (uint64_t)(uintptr_t)b->report_slot[bi],
                               b->ep_mps, TRB_TYPE(TR_NORMAL) | TRB_IOC);
    b->trb_buf[index] = bi;
}

static void hid_arm_batch(struct hid_binding *b) {
    for (unsigned i = 0; i < HID_BATCH; ++i)
        hid_push_trb(b, (uint8_t)i);
    doorbell(&controllers[b->controller], b->slot, b->ep_id);
}

static int hid_trb_index_of_param(struct hid_binding *b, uint64_t param,
                                  uint32_t *index) {
    uint64_t p = param & ~0xfULL;
    uint64_t base = b->ring.phys;
    uint64_t span = (uint64_t)b->ring.size * sizeof(struct trb);
    if (p >= base && (p - base) < span &&
        (((p - base) & (sizeof(struct trb) - 1u)) == 0)) {
        *index = (uint32_t)((p - base) / sizeof(struct trb));
        return 1;
    }
    return 0;
}

static uint8_t hid_slot_of_buffer(struct hid_binding *b, uint64_t param) {
    uint64_t p = param & ~0xfULL;
    for (unsigned i = 0; i < HID_BATCH; ++i)
        if ((uint64_t)(uintptr_t)b->report_slot[i] == p)
            return (uint8_t)i;
    return TRB_BUF_NONE;
}

static void hid_complete(struct hid_binding *b, uint64_t param) {
    uint32_t idx = b->deq;
    uint32_t pidx;
    uint8_t bi;

    if (idx >= b->ring.size - 1)
        idx = 0;
    bi = b->trb_buf[idx];

    if (hid_trb_index_of_param(b, param, &pidx) &&
        pidx < b->ring.size - 1 &&
        b->trb_buf[pidx] != TRB_BUF_NONE) {
        idx = pidx;
        bi = b->trb_buf[pidx];
    }
    if (bi == TRB_BUF_NONE)
        bi = hid_slot_of_buffer(b, param);
    if (bi == TRB_BUF_NONE || bi >= HID_BATCH)
        bi = 0;

    b->trb_buf[idx] = TRB_BUF_NONE;
    b->deq = idx + 1;
    if (b->deq == b->ring.size - 1)
        b->deq = 0;

    b->report = b->report_slot[bi];
    if (b->protocol == 1)
        hid_kbd_report(b);
    else
        hid_mouse_report(b);

    hid_push_trb(b, bi);
    doorbell(&controllers[b->controller], b->slot, b->ep_id);
}

static void pump_events(struct xhci_controller *h) {
    unsigned consumed = 0;

    while (consumed < h->ev_size) {
        volatile struct trb *trb = &h->ev_ring[h->ev_idx];
        uint64_t param = trb->param;
        uint32_t status = trb->status;
        uint32_t control = trb->control;
        uint32_t type, slot, epid;

        if ((control & TRB_CYCLE) != h->ev_ccs)
            break;
        type = (control >> TRB_TYPE_SHIFT) & 0x3f;
        slot = control >> 24;
        epid = (control >> 16) & 0x1f;

        ++h->ev_idx;
        if (h->ev_idx >= h->ev_size) {
            h->ev_idx = 0;
            h->ev_ccs ^= 1;
        }
        ++consumed;

        if (type == EV_COMMAND_COMPLETE) {
            h->cmd_done = 1;
            h->cmd_code = (uint8_t)(status >> 24);
            h->cmd_slot = (uint8_t)slot;
        } else if (type == EV_TRANSFER) {
            struct hid_binding *b = find_binding(h->index, slot, epid);
            if (b) {

                hid_complete(b, param);
            } else {

                h->xfer_done = 1;
                h->xfer_code = (uint8_t)(status >> 24);
                h->xfer_len = status & 0xffffff;
            }
        } else if (type == EV_PORT_STATUS_CHANGE) {
            (void)param;
        }
    }

    if (consumed) {
        uint64_t erdp = h->ev_phys + (uint64_t)h->ev_idx * sizeof(struct trb);
        uint32_t hi = (uint32_t)(erdp >> 32);
        mmio_write32(h->base, h->rt + 0x38, (uint32_t)erdp | ERDP_EHB);
        if (hi != h->erdp_hi) {
            mmio_write32(h->base, h->rt + 0x3c, hi);
            h->erdp_hi = hi;
        }
    }
}

static int wait_cmd(struct xhci_controller *h) {
    uint64_t deadline = read_tsc() + 2000000000ULL;
    while (!h->cmd_done) {
        pump_events(h);
        if (h->cmd_done)
            break;
        if (read_tsc() > deadline)
            return 0;
        __asm__ volatile("pause");
    }
    return h->cmd_code == CC_SUCCESS;
}

static int issue_command(struct xhci_controller *h, uint32_t type,
                         uint64_t param, uint32_t flags) {
    h->cmd_done = 0;
    h->cmd_code = 0;
    h->cmd_slot = 0;
    ring_push(&h->cmd, param, 0, TRB_TYPE(type) | flags);
    doorbell(h, 0, 0);
    return wait_cmd(h);
}

static void ctx_write(uint8_t *ctx, unsigned word, uint32_t value) {
    *(volatile uint32_t *)(ctx + word * 4) = value;
}

static uint8_t *ictx_slot(struct xhci_controller *h) {
    return h->ictx + h->stride;
}

static uint8_t *ictx_ep(struct xhci_controller *h, unsigned epid) {
    return h->ictx + h->stride * (epid + 1);
}

static void ctx_clear(uint8_t *ctx, uint32_t stride) {
    for (uint32_t i = 0; i < stride; ++i)
        ctx[i] = 0;
}

static void build_slot_ctx(struct xhci_controller *h, unsigned entries,
                           unsigned port, unsigned speed) {
    uint8_t *slot = ictx_slot(h);
    ctx_clear(slot, h->stride);
    ctx_write(slot, 0, (speed << 20) | (entries << 27));
    ctx_write(slot, 1, port << 16);
    ctx_write(slot, 2, 0);
    ctx_write(slot, 3, 0);
}

static void build_ep0_ctx(struct xhci_controller *h, unsigned max_packet,
                          uint64_t ring_phys) {
    uint8_t *ep = ictx_ep(h, 1);
    ctx_clear(ep, h->stride);
    ctx_write(ep, 0, 0);

    ctx_write(ep, 1, (max_packet << 16) | (3 << 1) | (4 << 3));
    ctx_write(ep, 2, (uint32_t)ring_phys | 1);
    ctx_write(ep, 3, 0);
    ctx_write(ep, 4, 0);
}

static void build_interrupt_ep_ctx(struct xhci_controller *h, unsigned ep_id,
                                   unsigned mps, uint64_t ring_phys) {
    uint8_t *ep = ictx_ep(h, ep_id);
    ctx_clear(ep, h->stride);

    ctx_write(ep, 0, 3u << 16);

    ctx_write(ep, 1, ((uint32_t)mps << 16) | (3u << 1) | (7u << 3));
    ctx_write(ep, 2, (uint32_t)ring_phys | 1u);
    ctx_write(ep, 3, 0);
    ctx_write(ep, 4, 0);
}

static int control_transfer(struct xhci_controller *h, uint8_t request_type,
                            uint8_t request, uint16_t value, uint16_t index,
                            uint16_t length, void *buffer) {
    uint8_t *setup = h->setup;
    setup[0] = request_type;
    setup[1] = request;
    setup[2] = (uint8_t)value;
    setup[3] = (uint8_t)(value >> 8);
    setup[4] = (uint8_t)index;
    setup[5] = (uint8_t)(index >> 8);
    setup[6] = (uint8_t)length;
    setup[7] = (uint8_t)(length >> 8);
    uint64_t param = 0;
    for (int i = 7; i >= 0; --i)
        param = (param << 8) | setup[i];

    unsigned in = (request_type & 0x80) != 0;
    uint32_t trt = length ? (in ? 3u : 2u) : 0u;
    h->xfer_done = 0;
    h->xfer_code = 0;
    ring_push(&h->ep0, param, 8,
              TRB_TYPE(TR_SETUP) | TRB_IDT | TRB_CH | (trt << 16));
    if (length)
        ring_push(&h->ep0, (uint64_t)(uintptr_t)buffer, length,
                  TRB_TYPE(TR_DATA) | TRB_CH | (in ? TRB_DIR : 0));

    uint32_t status_dir = (length && in) ? 0 : TRB_DIR;
    ring_push(&h->ep0, 0, 0, TRB_TYPE(TR_STATUS) | TRB_IOC | status_dir);
    doorbell(h, h->slot, 1);

    uint64_t deadline = read_tsc() + 2000000000ULL;
    while (!h->xfer_done) {
        pump_events(h);
        if (h->xfer_done)
            break;
        if (read_tsc() > deadline)
            return 0;
        __asm__ volatile("pause");
    }
    return h->xfer_code == CC_SUCCESS;
}

static int port_reset(struct xhci_controller *h, uint32_t reg) {
    uint32_t sc = mmio32(h->base, reg);
    if (h->ppc && !(sc & XHCI_PORT_PP)) {
        mmio_write32(h->base, reg, XHCI_PORT_PP);
        sc = mmio32(h->base, reg);
    }
    mmio_write32(h->base, reg, XHCI_PORT_PP | XHCI_PORT_PR);
    uint64_t deadline = read_tsc() + 2000000000ULL;
    for (;;) {
        sc = mmio32(h->base, reg);
        if (sc & XHCI_PORT_PED)
            break;
        if (!(sc & XHCI_PORT_CCS))
            return 0;

        if (sc & ((1u << 18) | (1u << 19) | (1u << 20)))
            return 0;
        if (read_tsc() > deadline)
            return 0;
        __asm__ volatile("pause");
    }
    mmio_write32(h->base, reg, XHCI_PORT_PP | XHCI_PORT_CLEAR);
    sc = mmio32(h->base, reg);
    return (sc & XHCI_PORT_CCS) != 0;
}

static unsigned port_speed(struct xhci_controller *h, uint32_t reg) {
    return mmio32(h->base, reg) & XHCI_PORT_PS_MASK;
}

static unsigned ep0_packet_for_speed(unsigned speed) {
    if (speed == 4)
        return 512;
    if (speed == 3)
        return 64;
    return 8;
}

static void kbd_push(struct hid_binding *b, char c) {
    uint8_t next = (uint8_t)((b->char_head + 1) & 63);
    if (next == b->char_tail)
        return;
    b->char_ring[b->char_head] = (uint8_t)c;
    b->char_head = next;
}

static const char hid_plain[128] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e',
    [0x09] = 'f', [0x0a] = 'g', [0x0b] = 'h', [0x0c] = 'i', [0x0d] = 'j',
    [0x0e] = 'k', [0x0f] = 'l', [0x10] = 'm', [0x11] = 'n', [0x12] = 'o',
    [0x13] = 'p', [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1a] = 'w', [0x1b] = 'x', [0x1c] = 'y',
    [0x1d] = 'z', [0x1e] = '1', [0x1f] = '2', [0x20] = '3', [0x21] = '4',
    [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8', [0x26] = '9',
    [0x27] = '0', [0x28] = '\n', [0x2a] = '\b', [0x2b] = '\t', [0x2c] = ' ',
    [0x2d] = '-', [0x2e] = '=', [0x2f] = '[', [0x30] = ']', [0x31] = '\\',
    [0x33] = ';', [0x34] = '\'', [0x35] = '`', [0x36] = ',', [0x37] = '.',
    [0x38] = '/'};

static const char hid_shifted[128] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E',
    [0x09] = 'F', [0x0a] = 'G', [0x0b] = 'H', [0x0c] = 'I', [0x0d] = 'J',
    [0x0e] = 'K', [0x0f] = 'L', [0x10] = 'M', [0x11] = 'N', [0x12] = 'O',
    [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1a] = 'W', [0x1b] = 'X', [0x1c] = 'Y',
    [0x1d] = 'Z', [0x1e] = '!', [0x1f] = '@', [0x20] = '#', [0x21] = '$',
    [0x22] = '%', [0x23] = '^', [0x24] = '&', [0x25] = '*', [0x26] = '(',
    [0x27] = ')', [0x28] = '\n', [0x2a] = '\b', [0x2b] = '\t', [0x2c] = ' ',
    [0x2d] = '_', [0x2e] = '+', [0x2f] = '{', [0x30] = '}', [0x31] = '|',
    [0x33] = ':', [0x34] = '"', [0x35] = '~', [0x36] = '<', [0x37] = '>',
    [0x38] = '?'};

static int report_has(const uint8_t *report, uint8_t usage) {
    for (unsigned i = 2; i < 8; ++i)
        if (report[i] == usage)
            return 1;
    return 0;
}

static const uint8_t hid_set1_letters[26] = {
    0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24,
    0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14,
    0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};

static uint8_t hid_usage_to_set1(uint8_t usage) {
    if (usage >= 0x04 && usage <= 0x1d)
        return hid_set1_letters[usage - 0x04];
    if (usage >= 0x1e && usage <= 0x26)
        return (uint8_t)(0x02 + (usage - 0x1e));
    if (usage == 0x27)
        return 0x0b;
    switch (usage) {
    case 0x28: return 0x1c;
    case 0x29: return 0x01;
    case 0x2a: return 0x0e;
    case 0x2b: return 0x0f;
    case 0x2c: return 0x39;
    case 0x2d: return 0x0c;
    case 0x2e: return 0x0d;
    case 0x2f: return 0x1a;
    case 0x30: return 0x1b;
    case 0x31: return 0x2b;
    case 0x33: return 0x27;
    case 0x34: return 0x28;
    case 0x35: return 0x29;
    case 0x36: return 0x33;
    case 0x37: return 0x34;
    case 0x38: return 0x35;
    case 0x39: return 0x3a;
    case 0x4f: return 0x4d;
    case 0x50: return 0x4b;
    case 0x51: return 0x50;
    case 0x52: return 0x48;
    case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f:
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
        return (uint8_t)(0x3b + (usage - 0x3a));
    case 0xe0: return 0x1d;
    case 0xe1: return 0x2a;
    case 0xe2: return 0x38;
    case 0xe4: return 0x1d;
    case 0xe5: return 0x36;
    case 0xe6: return 0x38;
    default: return 0;
    }
}

static void hid_kbd_report(struct hid_binding *b) {
    uint8_t report[8];
    for (unsigned i = 0; i < 8; ++i)
        report[i] = b->report[i];
    uint8_t modifiers = report[0];
    uint16_t mods = 0;
    if (modifiers & 0x22)
        mods |= KNX_KEYMOD_SHIFT;
    if (modifiers & 0x11)
        mods |= KNX_KEYMOD_CTRL;
    if (modifiers & 0x44)
        mods |= KNX_KEYMOD_ALT;
    knx_key_mods_set(mods);
    if (!b->first_key_beep) {
        b->first_key_beep = 1;
        knx_pcspk_beep(600, 90);
    }

    for (unsigned i = 2; i < 8; ++i) {
        uint8_t usage = b->prev_report[i];
        if (!usage || report_has(report, usage))
            continue;
        uint8_t code = hid_usage_to_set1(usage);
        if (code)
            knx_key_event_emit(code, 0);
    }
    for (unsigned i = 2; i < 8; ++i) {
        uint8_t usage = report[i];
        if (!usage || usage > 127)
            continue;
        if (report_has(b->prev_report, usage))
            continue;
        uint8_t code = hid_usage_to_set1(usage);
        if (code)
            knx_key_event_emit(code, 1);
        char c = (modifiers & 0x22) ? hid_shifted[usage] : hid_plain[usage];
        if (c)
            kbd_push(b, c);
    }
    for (unsigned i = 0; i < 8; ++i)
        b->prev_report[i] = report[i];

}

static void hid_mouse_report(struct hid_binding *b) {
    uint8_t buttons = b->report[0] & 0x07;
    int32_t dx = (int32_t)(int8_t)b->report[1];
    int32_t dy = (int32_t)(int8_t)b->report[2];
    knx_mouse_event_emit(dx, -dy, buttons);

}

struct hid_iface {
    uint8_t number;
    uint8_t ep_addr;
    uint16_t ep_mps;
    uint8_t protocol;
};

static int parse_config(const uint8_t *cfg, uint16_t total,
                        uint8_t *config_value, struct hid_iface *ifaces,
                        unsigned max_ifaces) {
    unsigned offset = 0, count = 0;
    uint8_t iface_num = 0, cls = 0, sub = 0, proto = 0;
    *config_value = 0;
    if (total > 512)
        total = 512;
    while (offset + 2 <= total) {
        uint8_t len = cfg[offset];
        uint8_t type = cfg[offset + 1];
        if (len < 2 || offset + len > total)
            break;
        if (type == 4 && len >= 9) {
            iface_num = cfg[offset + 2];
            cls = cfg[offset + 5];
            sub = cfg[offset + 6];
            proto = cfg[offset + 7];
        } else if (type == 5 && len >= 7) {
            uint8_t addr = cfg[offset + 2];
            uint8_t attr = cfg[offset + 3];
            if ((addr & 0x80) && ((attr & 3) == 3) &&
                cls == 3 && sub == 1 && (proto == 1 || proto == 2) &&
                count < max_ifaces) {
                ifaces[count].number = iface_num;
                ifaces[count].ep_addr = addr;
                ifaces[count].ep_mps =
                    (uint16_t)(cfg[offset + 4] | (cfg[offset + 5] << 8));
                ifaces[count].protocol = proto;
                ++count;
            }
        }
        offset += len;
    }
    *config_value = cfg[5];
    return (int)count;
}

static struct hid_binding *alloc_binding(void) {
    for (unsigned i = 0; i < MAX_BINDINGS; ++i) {
        if (!bindings[i].in_use) {
            bindings[i].in_use = 1;
            bindings[i].ready = 0;
            return &bindings[i];
        }
    }
    return 0;
}

static int enumerate_device(struct xhci_controller *h, unsigned port) {
    uint32_t reg = h->op + 0x400 + 0x10 * (port - 1);
    if (!port_reset(h, reg))
        return 0;
    unsigned speed = port_speed(h, reg);
    unsigned initial_packet = ep0_packet_for_speed(speed);

    h->slot = 0;
    h->probe_no_hid = 0;
    if (!issue_command(h, TR_ENABLE_SLOT, 0, 0) || !h->cmd_slot)
        return 0;
    h->slot = h->cmd_slot;

    uint8_t *octx = (uint8_t *)alloc_page();
    if (!octx)
        return 0;
    *(volatile uint64_t *)(h->dcbaap + 8 * h->slot) = (uint64_t)(uintptr_t)octx;
    __asm__ volatile("" ::: "memory");

    h->ictx[0] = 0;
    h->ictx[1] = 0;
    h->ictx[2] = 0;
    h->ictx[3] = 0;
    ctx_write(h->ictx, 1, 0x3);
    build_slot_ctx(h, 1, port, speed);
    build_ep0_ctx(h, initial_packet, h->ep0.phys);
    ring_reset(&h->ep0);

    if (!issue_command(h, TR_ADDRESS_DEVICE, (uint64_t)(uintptr_t)h->ictx,
                       TRB_BSR | ((uint32_t)h->slot << 24)))
        return 0;

    if (!control_transfer(h, 0x80, 6, 0x0100, 0, 8, h->xfer))
        return 0;
    unsigned max_packet = h->xfer[7];
    if (speed != 4 && max_packet)
        initial_packet = max_packet;

    h->ictx[0] = 0;
    h->ictx[1] = 0;
    h->ictx[2] = 0;
    h->ictx[3] = 0;
    ctx_write(h->ictx, 1, 0x3);
    build_slot_ctx(h, 1, port, speed);
    build_ep0_ctx(h, initial_packet, h->ep0.phys);
    ring_reset(&h->ep0);
    if (!issue_command(h, TR_ADDRESS_DEVICE, (uint64_t)(uintptr_t)h->ictx,
                       (uint32_t)h->slot << 24))
        return 0;

    if (!control_transfer(h, 0x80, 6, 0x0100, 0, 18, h->xfer))
        return 0;

    if (!control_transfer(h, 0x80, 6, 0x0200, 0, 9, h->xfer))
        return 0;
    uint16_t total = (uint16_t)(h->xfer[2] | (h->xfer[3] << 8));
    if (total < 9 || total > 512)
        return 0;
    if (!control_transfer(h, 0x80, 6, 0x0200, 0, total, h->xfer))
        return 0;

    struct hid_iface ifaces[MAX_IFACES];
    uint8_t config_value;
    int nic = parse_config(h->xfer, total, &config_value, ifaces, MAX_IFACES);
    if (nic <= 0) {

        h->probe_no_hid = 1;
        return 0;
    }

    if (!control_transfer(h, 0x00, 9, config_value, 0, 0, 0))
        return 0;

    struct hid_binding *bs[MAX_IFACES] = {0};
    unsigned add_flags = 1u;
    unsigned max_ep_id = 1;

    for (int i = 0; i < nic; ++i) {
        struct hid_binding *b = alloc_binding();
        if (!b)
            goto fail;
        b->controller = h->index;
        b->slot = h->slot;
        b->ep_num = (uint8_t)(ifaces[i].ep_addr & 0x0f);
        b->ep_id = (uint8_t)(b->ep_num * 2 + 1);
        b->ep_mps = ifaces[i].ep_mps ? (uint16_t)(ifaces[i].ep_mps & 0x7ff) : 8;
        if (!b->ep_mps || b->ep_mps > 8)
            b->ep_mps = 8;
        b->protocol = ifaces[i].protocol;
        b->interface_number = ifaces[i].number;
        b->reports = (uint8_t *)alloc_page();
        if (!alloc_ring_page(&b->ring) || !b->reports) {
            b->in_use = 0;
            goto fail;
        }

        for (unsigned j = 0; j < HID_BATCH; ++j)
            b->report_slot[j] = b->reports + j * HID_REPORT_SLOT;
        for (unsigned j = 0; j < RING_TRBS; ++j)
            b->trb_buf[j] = TRB_BUF_NONE;
        b->deq = 0;
        b->report = b->report_slot[0];
        for (unsigned j = 0; j < 8; ++j)
            b->prev_report[j] = 0;
        b->char_head = 0;
        b->char_tail = 0;
        b->first_key_beep = 0;
        bs[i] = b;

        add_flags |= 1u << b->ep_id;
        if (b->ep_id > max_ep_id)
            max_ep_id = b->ep_id;
    }

    for (int i = 0; i < nic; ++i)
        (void)control_transfer(h, 0x21, 0x0b, 0, bs[i]->interface_number, 0, 0);

    for (unsigned w = 0; w < 1024; ++w)
        ctx_write(h->ictx, w, 0);
    ctx_write(h->ictx, 1, add_flags);
    build_slot_ctx(h, max_ep_id, port, speed);
    for (int i = 0; i < nic; ++i)
        build_interrupt_ep_ctx(h, bs[i]->ep_id, bs[i]->ep_mps, bs[i]->ring.phys);

    if (!issue_command(h, TR_CONFIGURE_ENDPOINT, (uint64_t)(uintptr_t)h->ictx,
                       (uint32_t)h->slot << 24))
        goto fail;

    for (int i = 0; i < nic; ++i) {
        hid_arm_batch(bs[i]);
        bs[i]->ready = 1;
    }

    knx_pcspk_beep(1200, 180);
    return nic;

fail:
    for (int i = 0; i < nic; ++i) {
        if (bs[i]) {
            bs[i]->ready = 0;
            bs[i]->in_use = 0;
        }
    }
    return 0;
}

static int start_controller(struct xhci_controller *h) {
    h->op = h->base[0];
    uint32_t hccparams1 = mmio32(h->base, 0x10);
    h->csz = (uint8_t)((hccparams1 >> 2) & 1);
    h->stride = 32u << h->csz;
    h->db = mmio32(h->base, 0x14) & ~3u;
    h->rt = mmio32(h->base, 0x18) & ~3u;
    if (h->op < 0x20 || h->op > 0x100 || !h->db || !h->rt) {
        knx_log("[XHCI]", "Capability registers are unusable");
        return 0;
    }

    if (!alloc_ring_page(&h->cmd) || !alloc_ring_page(&h->ep0)) {
        knx_log("[XHCI]", "Out of memory for transfer rings");
        return 0;
    }
    uint8_t *ev = (uint8_t *)alloc_page();
    if (!ev) {
        knx_log("[XHCI]", "Out of memory for the event ring");
        return 0;
    }
    h->ev_ring = (struct trb *)ev;
    h->ev_phys = (uint64_t)(uintptr_t)ev;
    h->ev_size = EVENT_TRBS;
    h->ev_idx = 0;
    h->ev_ccs = 1;
    h->erst = (uint8_t *)alloc_page();
    h->dcbaap = (uint8_t *)alloc_page();
    h->ictx = (uint8_t *)alloc_page();
    h->setup = (uint8_t *)alloc_page();
    h->xfer = (uint8_t *)alloc_page();
    if (!h->erst || !h->dcbaap || !h->ictx || !h->setup || !h->xfer) {
        knx_log("[XHCI]", "Out of memory for controller structures");
        return 0;
    }

    knx_pci_device pci = {0};
    pci.bus = h->bus;
    pci.device = h->dev;
    pci.function = h->fn;
    knx_pci_enable_device(&pci, KNX_PCI_ENABLE_MEMORY | KNX_PCI_ENABLE_BUS_MASTER);

    mmio_write32(h->base, h->op, USBCMD_HCRST);
    uint64_t deadline = read_tsc() + 2000000000ULL;
    while (mmio32(h->base, h->op) & USBCMD_HCRST) {
        if (read_tsc() > deadline) {
            knx_log("[XHCI]", "Host controller reset timed out");
            return 0;
        }
        __asm__ volatile("pause");
    }

    mmio_write32(h->base, h->op + 0x38, h->max_slots);
    mmio_write64(h->base, h->op + 0x30, (uint64_t)(uintptr_t)h->dcbaap);
    mmio_write32(h->base, h->op + 0x18, (uint32_t)h->cmd.phys | 1u);
    mmio_write32(h->base, h->op + 0x1c, 0);

    *(volatile uint32_t *)(h->erst + 0) = (uint32_t)h->ev_phys;
    *(volatile uint32_t *)(h->erst + 4) = (uint32_t)(h->ev_phys >> 32);
    *(volatile uint32_t *)(h->erst + 8) = h->ev_size;
    *(volatile uint32_t *)(h->erst + 12) = 0;
    mmio_write32(h->base, h->rt + 0x28, 1);
    mmio_write32(h->base, h->rt + 0x30, (uint32_t)(uintptr_t)h->erst);
    mmio_write32(h->base, h->rt + 0x34, 0);
    mmio_write32(h->base, h->rt + 0x38,
                 (uint32_t)h->ev_phys | ERDP_EHB);
    h->erdp_hi = (uint32_t)(h->ev_phys >> 32);
    mmio_write32(h->base, h->rt + 0x3c, h->erdp_hi);

    mmio_write32(h->base, h->rt + 0x20, 0);

    mmio_write32(h->base, h->op, USBCMD_RS);
    deadline = read_tsc() + 2000000000ULL;
    while (mmio32(h->base, h->op + 4) & USBSTS_HCH) {
        if (read_tsc() > deadline) {
            knx_log("[XHCI]", "Host controller did not start");
            return 0;
        }
        __asm__ volatile("pause");
    }

    h->started = 1;
    h->halted = 0;

    knx_pcspk_beep(1000, 150);
    knx_log("[XHCI]", "Host controller running");
    return 1;
}

int knx_xhci_probe(const knx_pci_device *device) {
    if (!device || controller_count >= MAX_XHCI)
        return 0;
    uint64_t physical = knx_pci_mmio_bar(device, 0);
    if (!physical) {
        knx_log("[XHCI]", "No MMIO BAR, rejecting controller");
        return 0;
    }

    if (knx_vm_map_mmio(physical, 0x10000) != 0) {
        knx_log("[XHCI]", "Cannot map the MMIO BAR");
        return 0;
    }

    if (knx_pci_enable_device(device, KNX_PCI_ENABLE_MEMORY) != 0) {
        knx_log("[XHCI]", "Cannot enable the PCI function");
        return 0;
    }

    uint32_t command = knx_pci_read32(device, 4);
    knx_pci_write32(device, 4, command & ~KNX_PCI_ENABLE_BUS_MASTER);
    volatile uint8_t *base = (volatile uint8_t *)(uintptr_t)physical;

    uint32_t cap0 = mmio32(base, 0);
    uint8_t cap_length = (uint8_t)cap0;
    uint16_t version = (uint16_t)(cap0 >> 16);

    uint32_t hcsparams1 = mmio32(base, 4);
    uint32_t max_ports = hcsparams1 >> 24;
    uint32_t max_slots = hcsparams1 & 0xff;
    if (cap_length < 0x20 || cap_length > 0x80 || max_ports > 64) {
        knx_log("[XHCI]", "Malformed capability registers");
        return 0;
    }
    uint32_t status = mmio32(base, cap_length + 4);

    unsigned idx = controller_count++;
    struct xhci_controller *controller = &controllers[idx];
    controller->index = (uint8_t)idx;
    controller->base = base;
    controller->bus = device->bus;
    controller->dev = device->device;
    controller->fn = device->function;
    controller->ports = (uint8_t)max_ports;
    controller->version = version;
    controller->halted = (status & XHCI_STS_HCHALTED) != 0;
    controller->max_slots = (uint8_t)max_slots;

    controller->ppc = (uint8_t)((hcsparams1 >> 4) & 1u);
    return 1;
}

void knx_usb_tick(void) {
    knx_legacy_usb_tick();
    for (unsigned c = 0; c < controller_count; ++c) {
        struct xhci_controller *h = &controllers[c];
        if (!h->started) {
            if (!start_controller(h))
                continue;
        }
        pump_events(h);

        for (unsigned p = 1; p <= h->ports; ++p) {
            unsigned idx = p - 1;
            if (idx >= 64)
                continue;
            uint32_t reg = h->op + 0x400 + 0x10 * idx;
            uint32_t sc = mmio32(h->base, reg);

            if (!(sc & XHCI_PORT_PP)) {

                mmio_write32(h->base, reg, XHCI_PORT_PP);
                sc = mmio32(h->base, reg);
                if (!h->pp_reported[idx] && !(sc & XHCI_PORT_PP)) {
                    h->pp_reported[idx] = 1;
                    knx_puts("[XHCI] port ");
                    knx_print_u64(p);
                    knx_puts(" PP stuck: PORTSC=0x");
                    knx_print_u64(sc);
                    knx_puts("\n");
                }
            }

            uint8_t ccs = (uint8_t)(sc & XHCI_PORT_CCS);
            if (ccs != h->port_ccs[idx]) {
                h->port_ccs[idx] = ccs;
                h->port_scanned[idx] = 0;
                h->port_cooldown[idx] = 0;
            }
            if (!ccs || h->port_scanned[idx])
                continue;
            if (h->port_cooldown[idx]) {
                --h->port_cooldown[idx];
                continue;
            }

            int rc = enumerate_device(h, p);
            if (rc > 0) {
                h->port_scanned[idx] = 1;
            } else if (h->probe_no_hid) {
                h->port_scanned[idx] = 1;
                h->port_cooldown[idx] = 200;
            } else {
                h->port_cooldown[idx] = 50;
            }
        }
    }
}

char knx_usbkbd_read(void) {
    char legacy = knx_legacy_usbkbd_read();
    if (legacy)
        return legacy;
    for (unsigned i = 0; i < MAX_BINDINGS; ++i) {
        struct hid_binding *b = &bindings[i];
        if (!b->in_use || !b->ready || b->protocol != 1)
            continue;
        if (b->char_head == b->char_tail)
            continue;
        char ch = (char)b->char_ring[b->char_tail];
        b->char_tail = (uint8_t)((b->char_tail + 1) & 63);
        return ch;
    }
    return 0;
}

void knx_usb_info(void) {
    knx_puts("USB xHCI controllers: ");
    knx_print_u64(controller_count);
    knx_puts("\n");
    for (unsigned c = 0; c < controller_count; ++c) {
        struct xhci_controller *x = &controllers[c];
        knx_puts("  xHCI ");
        if (x->version) {
            knx_print_u64(x->version >> 8);
            knx_putc('.');
            knx_print_u64(x->version & 0xff);
        } else
            knx_puts("revision-unknown");
        knx_puts(" ");
        knx_puts(x->halted ? "halted" : "ready");
        knx_puts("; slots: ");
        knx_print_u64(x->max_slots);
        knx_puts("; ports:");
        uint32_t op = x->base[0];
        for (unsigned p = 0; p < x->ports; ++p) {
            uint32_t status = mmio32(x->base, op + 0x400 + p * 0x10);
            if (status & XHCI_PORT_CCS) {
                knx_putc(' ');
                knx_print_u64(p + 1);
                knx_puts(status & XHCI_PORT_PED ? "=enabled" : "=connected");
                if (!(status & XHCI_PORT_PP))
                    knx_puts("/unpowered");
            }
        }
        knx_puts("\n");
    }
    for (unsigned i = 0; i < MAX_BINDINGS; ++i) {
        struct hid_binding *b = &bindings[i];
        if (!b->in_use || !b->ready)
            continue;
        knx_puts("  USB HID ");
        knx_puts(b->protocol == 1 ? "keyboard" : "mouse");
        knx_puts(": slot ");
        knx_print_u64(b->slot);
        knx_puts(" endpoint ");
        knx_print_u64(b->ep_num);
        knx_puts(" IN (if ");
        knx_print_u64(b->interface_number);
        knx_puts(")\n");
    }
    if (!controller_count)
        knx_puts("  No supported host controller detected.\n");
    else if (!controllers[0].started)
        knx_puts("PCI/MMIO ownership is active; the xHCI controller has not been started yet.\n");
}
