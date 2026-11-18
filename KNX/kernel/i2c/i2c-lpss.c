#include "knx.h"

#define LPSS_MAX_BUS 4
#define LPSS_REPORT_MAX 256
#define LPSS_HID_RETRIES 3

#define DW_IC_CON          0x00u
#define DW_IC_TAR          0x04u
#define DW_IC_DATA_CMD     0x10u
#define DW_IC_SS_HCNT      0x14u
#define DW_IC_SS_LCNT      0x18u
#define DW_IC_FS_HCNT      0x1cu
#define DW_IC_FS_LCNT      0x20u
#define DW_IC_INTR_STAT    0x2cu
#define DW_IC_INTR_MASK    0x30u
#define DW_IC_RAW_INTR_STAT 0x34u
#define DW_IC_RX_TL        0x38u
#define DW_IC_TX_TL        0x3cu
#define DW_IC_CLR_TX_ABRT  0x54u
#define DW_IC_ENABLE       0x6cu
#define DW_IC_STATUS       0x70u
#define DW_IC_TXFLR        0x74u
#define DW_IC_RXFLR        0x78u
#define DW_IC_SDA_HOLD     0x7cu
#define DW_IC_TX_ABRT_SOURCE 0x80u
#define DW_IC_ENABLE_STATUS 0x9cu
#define DW_IC_COMP_TYPE    0xfcu

#define DW_IC_ENABLE_ENABLE 0x01u

#define DW_CMD_READ  0x100u
#define DW_CMD_STOP  0x200u
#define DW_CMD_RESTART 0x400u
#define DW_INTR_TX_ABRT 0x40u
#define DW_STATUS_ACTIVITY 0x01u

#define LPSS_PRIV_RESETS 0x04u
#define LPSS_PRIV_REMAP  0x40u

#define LPSS_IC_CLK_HZ 133000000ULL

struct lpss_bus {
    int used;
    volatile uint8_t *reg;
    volatile uint8_t *priv;
    uint8_t addr;
    uint16_t vid, pid;
    uint16_t report_reg, cmd_reg, data_reg;
    uint8_t report[LPSS_REPORT_MAX];
    uint16_t report_len;
    uint8_t report_id;
    uint16_t x_bit, y_bit, tip_bit;
    uint8_t x_size, y_size, tip_size;
    int have_xy;
    int32_t last_x, last_y;
    int last_tip;
    uint8_t failures;
    uint64_t touch_start;
    uint64_t next_poll;
};
static struct lpss_bus buses[LPSS_MAX_BUS];

static inline uint32_t rd32(volatile uint8_t *base, unsigned off) {
    return *(volatile uint32_t *)(void *)(base + off);
}
static inline void wr32(volatile uint8_t *base, unsigned off, uint32_t value) {
    *(volatile uint32_t *)(void *)(base + off) = value;
}

static void delay_cycles(volatile unsigned n) {
    while (n--)
        __asm__ volatile("pause");
}

static uint32_t scl_hcnt(uint32_t thigh_ns, uint32_t tf_ns) {
    return (uint32_t)((LPSS_IC_CLK_HZ * (uint64_t)(thigh_ns + tf_ns) / 1000000000ULL) - 3u);
}
static uint32_t scl_lcnt(uint32_t tlow_ns, uint32_t tf_ns) {
    return (uint32_t)((LPSS_IC_CLK_HZ * (uint64_t)(tlow_ns + tf_ns) / 1000000000ULL) - 1u);
}

static void dw_disable(struct lpss_bus *b) {
    wr32(b->reg, DW_IC_ENABLE, 0);
    for (unsigned i = 0; i < 100; ++i) {
        if (!(rd32(b->reg, DW_IC_ENABLE_STATUS) & DW_IC_ENABLE_ENABLE))
            return;
        delay_cycles(2000);
    }
}

static int dw_init(struct lpss_bus *b) {
    uint32_t type = rd32(b->reg, DW_IC_COMP_TYPE);
    if (type != 0x44570140u && (type & 0xffffu) != 0x0140u)
        return 0;
    dw_disable(b);

    wr32(b->reg, DW_IC_SS_HCNT, scl_hcnt(4000, 171));
    wr32(b->reg, DW_IC_SS_LCNT, scl_lcnt(4700, 208));
    wr32(b->reg, DW_IC_FS_HCNT, scl_hcnt(600, 171));
    wr32(b->reg, DW_IC_FS_LCNT, scl_lcnt(1300, 208));
    wr32(b->reg, DW_IC_SDA_HOLD, 42);
    wr32(b->reg, DW_IC_TAR, 0);
    wr32(b->reg, DW_IC_TX_TL, 0);
    wr32(b->reg, DW_IC_RX_TL, 0);
    wr32(b->reg, DW_IC_INTR_MASK, 0);

    wr32(b->reg, DW_IC_CON, 0x01u | 0x40u | 0x20u);
    wr32(b->reg, DW_IC_ENABLE, DW_IC_ENABLE_ENABLE);
    for (unsigned i = 0; i < 100; ++i) {
        if (rd32(b->reg, DW_IC_ENABLE_STATUS) & DW_IC_ENABLE_ENABLE)
            return 1;
        delay_cycles(2000);
    }
    return 0;
}

static int dw_xfer(struct lpss_bus *b, uint8_t addr,
                   const uint8_t *tx, uint32_t txlen,
                   uint8_t *rx, uint32_t rxlen) {
    if (rd32(b->reg, DW_IC_STATUS) & DW_STATUS_ACTIVITY)
        return -1;
    dw_disable(b);
    wr32(b->reg, DW_IC_TAR, addr);
    (void)rd32(b->reg, DW_IC_CLR_TX_ABRT);
    wr32(b->reg, DW_IC_ENABLE, DW_IC_ENABLE_ENABLE);

    for (uint32_t i = 0; i < txlen; ++i)
        wr32(b->reg, DW_IC_DATA_CMD, tx[i]);
    for (uint32_t i = 0; i < rxlen; ++i) {
        uint32_t cmd = DW_CMD_READ;
        if (i == 0)
            cmd |= DW_CMD_RESTART;
        if (i + 1 == rxlen)
            cmd |= DW_CMD_STOP;
        wr32(b->reg, DW_IC_DATA_CMD, cmd);
    }

    volatile uint32_t tries = 500000u;
    while (tries-- && rd32(b->reg, DW_IC_RXFLR) < rxlen) {
        if (rd32(b->reg, DW_IC_RAW_INTR_STAT) & DW_INTR_TX_ABRT)
            break;
        __asm__ volatile("pause");
    }
    if (rxlen && rd32(b->reg, DW_IC_RXFLR) < rxlen) {
        (void)rd32(b->reg, DW_IC_TX_ABRT_SOURCE);
        dw_disable(b);
        return -1;
    }
    for (uint32_t i = 0; i < rxlen; ++i)
        rx[i] = (uint8_t)rd32(b->reg, DW_IC_DATA_CMD);
    if (rd32(b->reg, DW_IC_RAW_INTR_STAT) & DW_INTR_TX_ABRT) {
        (void)rd32(b->reg, DW_IC_TX_ABRT_SOURCE);
        dw_disable(b);
        return -1;
    }
    dw_disable(b);
    return 0;
}

static int hid_command(struct lpss_bus *b, uint8_t report_type_id, uint8_t opcode) {
    uint8_t cmd[4] = {
        (uint8_t)(b->cmd_reg & 0xff), (uint8_t)(b->cmd_reg >> 8),
        report_type_id, opcode
    };
    for (int attempt = 0; attempt < LPSS_HID_RETRIES; ++attempt)
        if (dw_xfer(b, b->addr, cmd, sizeof(cmd), 0, 0) == 0)
            return 0;
    return -1;
}

static int read_reg(struct lpss_bus *b, uint16_t offset, uint8_t *dst, uint32_t len) {
    uint8_t req[2] = {(uint8_t)(offset & 0xff), (uint8_t)(offset >> 8)};
    for (int attempt = 0; attempt < LPSS_HID_RETRIES; ++attempt)
        if (dw_xfer(b, b->addr, req, 2, dst, len) == 0)
            return 0;
    return -1;
}

struct hid_parse {
    uint16_t usage_page;
    uint16_t report_id;
    uint8_t size;
    uint8_t count;
    uint16_t bit;
    uint32_t usages[16];
    unsigned nusages;
};

static void hid_parse_report(struct lpss_bus *b) {
    struct hid_parse p = {0};
    const uint8_t *d = b->report;
    unsigned i = 0;
    b->have_xy = 0;
    while (i < b->report_len) {
        uint8_t prefix = d[i++];
        uint8_t size = prefix & 3u;
        if (size == 3)
            size = 4;
        if (i + size > b->report_len)
            break;
        uint32_t value = 0;
        for (uint8_t k = 0; k < size; ++k)
            value |= (uint32_t)d[i + k] << (8u * k);
        i += size;
        uint8_t type = (uint8_t)((prefix >> 2) & 3u);
        uint8_t tag = (uint8_t)(prefix >> 4);
        if (type == 1) {
            if (tag == 0) p.usage_page = (uint16_t)value;
            else if (tag == 4) { p.report_id = (uint16_t)value; p.bit = 0; }
            else if (tag == 7) p.size = (uint8_t)value;
            else if (tag == 9) p.count = (uint8_t)value;
        } else if (type == 2) {
            if (tag == 0 && p.nusages < 16)
                p.usages[p.nusages++] = ((uint32_t)p.usage_page << 16) | value;
        } else if (type == 0 && tag == 8) {
            for (unsigned u = 0; u < p.nusages; ++u) {
                uint32_t usage = p.usages[u];
                if (usage == 0x000d0030u && !b->have_xy) {
                    b->x_bit = p.bit; b->x_size = p.size;
                } else if (usage == 0x000d0031u && !b->have_xy) {
                    b->y_bit = p.bit; b->y_size = p.size;
                    b->have_xy = (b->x_size != 0);
                } else if (usage == 0x000d0042u && !b->tip_bit) {
                    b->tip_bit = p.bit; b->tip_size = p.size;
                }
                p.bit = (uint16_t)(p.bit + p.size);
            }
            p.nusages = 0;
        }
    }
    if (b->x_size && b->y_size)
        b->have_xy = 1;
    if (b->have_xy)
        knx_log("I2C-HID", "touchpad digitizer X/Y located in input report");
}

static int32_t get_bits(const uint8_t *r, uint16_t bit, uint8_t size) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < size && i < 32; ++i) {
        uint32_t index = (uint32_t)(bit + i) / 8u;
        if (index >= LPSS_REPORT_MAX)
            break;
        if (r[index] & (1u << ((bit + i) & 7u)))
            v |= 1u << i;
    }
    if (size == 16 && (v & 0x8000u)) return (int32_t)v - 0x10000;
    if (size == 12 && (v & 0x0800u)) return (int32_t)v - 0x1000;
    return (int32_t)v;
}

static void hid_poll(struct lpss_bus *b) {
    uint8_t rx[LPSS_REPORT_MAX + 2];
    uint8_t report_id = (uint8_t)(b->report_id ? b->report_id : 1);
    int ok = hid_command(b, (uint8_t)((1u << 4) | report_id), 0x02) == 0;

    if (ok)
        ok = read_reg(b, b->data_reg, rx, (uint32_t)b->report_len + 2u) == 0;
    if (!ok) {

        if (++b->failures >= 8) {
            knx_puts("[I2C-HID] touchpad stopped answering; polling disabled\n");
            b->addr = 0;
        }
        return;
    }
    b->failures = 0;
    const uint8_t *r = rx + 2 + (b->report_id ? 1 : 0);
    int tip = b->tip_size ? (get_bits(r, b->tip_bit, b->tip_size) != 0) : 1;
    if (!b->have_xy)
        return;
    int32_t x = get_bits(r, b->x_bit, b->x_size);
    int32_t y = get_bits(r, b->y_bit, b->y_size);
    if (tip && b->last_tip) {
        knx_mouse_event_emit(x - b->last_x, y - b->last_y, 0);
    } else if (tip && !b->last_tip) {
        b->touch_start = knx_ticks();
    } else if (!tip && b->last_tip) {
        if ((int64_t)(knx_ticks() - b->touch_start) < 20) {
            knx_mouse_event_emit(0, 0, KNX_MOUSE_BUTTON_LEFT);
            knx_mouse_event_emit(0, 0, 0);
        }
    }
    b->last_tip = tip;
    b->last_x = x;
    b->last_y = y;
}

static void hid_detect(struct lpss_bus *b) {
    static const uint8_t candidates[] = {
        0x15, 0x2a, 0x2c, 0x4a, 0x10, 0x20, 0x27, 0x38, 0x67
    };

    static const uint16_t desc_regs[] = {0x0001, 0x0000};
    uint8_t desc[30];
    for (unsigned i = 0; i < sizeof(candidates); ++i) {
        b->addr = candidates[i];
        for (unsigned r = 0; r < sizeof(desc_regs) / sizeof(desc_regs[0]); ++r) {
            if (read_reg(b, desc_regs[r], desc, sizeof(desc)) != 0)
                continue;
            uint16_t dlen = (uint16_t)(desc[0] | (desc[1] << 8));
            uint16_t rlen = (uint16_t)(desc[4] | (desc[5] << 8));
            if (dlen < 0x1e || dlen > sizeof(desc) || !rlen)
                continue;
            b->report_reg = (uint16_t)(desc[6] | (desc[7] << 8));
            b->cmd_reg = (uint16_t)(desc[16] | (desc[17] << 8));
            b->data_reg = (uint16_t)(desc[18] | (desc[19] << 8));
            b->vid = (uint16_t)(desc[20] | (desc[21] << 8));
            b->pid = (uint16_t)(desc[22] | (desc[23] << 8));
            b->report_len = (rlen <= sizeof(b->report)) ? rlen : sizeof(b->report);
            knx_puts("[I2C-HID] touchpad at 0x");
            knx_print_u64(b->addr);
            knx_puts(" vid=0x"); knx_print_u64(b->vid);
            knx_puts(" pid=0x"); knx_print_u64(b->pid);
            knx_puts(" report="); knx_print_u64(b->report_len); knx_puts(" bytes\n");
            if (read_reg(b, b->report_reg, b->report, b->report_len) == 0)
                hid_parse_report(b);
            if (!b->have_xy) {
                knx_puts("[I2C-HID] no digitizer X/Y in report descriptor; "
                         "touchpad left disabled\n");
                b->addr = 0;
                return;
            }
            if (hid_command(b, 0, 0x01) == 0)
                knx_log("I2C-HID", "touchpad reset acknowledged");
            b->next_poll = knx_ticks() + 20;
            return;
        }
    }
    b->addr = 0;
}

int knx_i2c_lpss_probe(const knx_pci_device *device) {
    if (!device || device->vendor_id != 0x8086)
        return 0;
    switch (device->device_id) {
    case 0x51c5: case 0x51c6:
    case 0x51d8: case 0x51d9:
    case 0x51e8: case 0x51e9:
    case 0x51ea: case 0x51eb:
        break;
    default:
        return 0;
    }
    if (knx_pci_enable_device(device, KNX_PCI_ENABLE_MEMORY | KNX_PCI_ENABLE_BUS_MASTER) != 0)
        return 0;
    uint64_t phys = knx_pci_mmio_bar(device, 0);
    if (!phys || knx_vm_map_mmio(phys, 0x1000) != 0)
        return 0;
    struct lpss_bus *b = 0;
    for (unsigned i = 0; i < LPSS_MAX_BUS; ++i)
        if (!buses[i].used) { b = &buses[i]; break; }
    if (!b)
        return 0;
    b->used = 1;
    b->reg = (volatile uint8_t *)(uintptr_t)phys;
    b->priv = (volatile uint8_t *)(uintptr_t)(phys + 0x200);
    wr32(b->priv, LPSS_PRIV_RESETS, 0x07);
    delay_cycles(10000);
    wr32(b->priv, LPSS_PRIV_REMAP, (uint32_t)phys);
    wr32(b->priv, LPSS_PRIV_REMAP + 4, (uint32_t)(phys >> 32));
    if (!dw_init(b)) {
        knx_puts("[I2C] 8086:");
        knx_print_u64(device->device_id);
        knx_puts(" is not a DesignWare I2C master\n");
        b->used = 0;
        return 0;
    }
    knx_puts("[I2C] Alder Lake Serial IO I2C at ");
    knx_print_u64(phys);
    knx_puts("\n");
    hid_detect(b);
    return 1;
}

void knx_i2c_tick(void) {
    uint64_t now = knx_ticks();
    for (unsigned i = 0; i < LPSS_MAX_BUS; ++i) {
        struct lpss_bus *b = &buses[i];
        if (!b->used || !b->addr)
            continue;
        if ((int64_t)(now - b->next_poll) >= 0) {
            b->next_poll = now + 5;
            hid_poll(b);
        }
    }
}
