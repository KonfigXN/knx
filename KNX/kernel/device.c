#include "knx.h"

#define MAX_DRIVERS 16
#define MAX_DEVICES 64

struct driver_entry {
    const char *name;
    uint8_t class_code, subclass, prog_if;
    knx_driver_probe_fn probe;
};
struct device_entry {
    knx_pci_device pci;
    const char *driver;
};
static struct driver_entry drivers[MAX_DRIVERS];
static struct device_entry devices[MAX_DEVICES];
static unsigned driver_count, device_count, matched_count;
static int builtin_drivers_registered;

static int same_text(const char *a, const char *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static void outl(uint16_t port, uint32_t value) {
    knx_io_out32(port, value);
}
static uint32_t inl(uint16_t port) {
    return knx_io_in32(port);
}
static uint32_t pci_config_read(unsigned bus, unsigned slot, unsigned function, unsigned offset) {
    uint32_t address = 0x80000000u | (bus << 16) | (slot << 11) | (function << 8) | (offset & 0xfc);
    outl(0xcf8, address);
    return inl(0xcfc);
}
static void pci_config_write(unsigned bus, unsigned slot, unsigned function, unsigned offset,
                             uint32_t value) {
    uint32_t address = 0x80000000u | (bus << 16) | (slot << 11) | (function << 8) | (offset & 0xfc);
    outl(0xcf8, address);
    outl(0xcfc, value);
}
uint32_t knx_pci_read32(const knx_pci_device *device, unsigned offset) {
    if (!device || offset >= 256)
        return 0xffffffffu;
    return pci_config_read(device->bus, device->device, device->function, offset);
}

void knx_pci_write32(const knx_pci_device *device, unsigned offset, uint32_t value) {
    if (!device || offset >= 256)
        return;
    uint32_t address = 0x80000000u | ((uint32_t)device->bus << 16) |
                       ((uint32_t)device->device << 11) | ((uint32_t)device->function << 8) |
                       (offset & 0xfcu);
    outl(0xcf8, address);
    outl(0xcfc, value);
}

int knx_pci_enable_device(const knx_pci_device *device, uint16_t flags) {
    if (!device || (flags & ~(KNX_PCI_ENABLE_IO | KNX_PCI_ENABLE_MEMORY |
                              KNX_PCI_ENABLE_BUS_MASTER)))
        return -1;
    uint32_t id = knx_pci_read32(device, 0);
    if ((uint16_t)id == 0xffff || (uint16_t)(id >> 16) == 0xffff)
        return -1;
    uint32_t command = knx_pci_read32(device, 4);
    command = (command & 0xffff0000u) | ((command & 0xffffu) | flags);
    knx_pci_write32(device, 4, command);
    return ((knx_pci_read32(device, 4) & flags) == flags) ? 0 : -1;
}

uint64_t knx_pci_mmio_bar(const knx_pci_device *device, unsigned index) {
    if (!device || index >= 6)
        return 0;
    uint32_t low = knx_pci_read32(device, 0x10 + index * 4);
    if (low == 0xffffffffu || !low || (low & 1))
        return 0;
    uint32_t type = (low >> 1) & 3;
    if (type != 0 && type != 2)
        return 0;
    uint64_t base = low & ~0x0fULL;
    if (type == 2) {
        if (index == 5)
            return 0;
        uint32_t high = knx_pci_read32(device, 0x14 + index * 4);
        if (high == 0xffffffffu)
            return 0;
        base |= (uint64_t)high << 32;
    }
    return base;
}

int knx_driver_register(const char *name, uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                        knx_driver_probe_fn probe) {
    if (!name || !probe || driver_count >= MAX_DRIVERS)
        return -1;
    for (unsigned i = 0; i < driver_count; ++i)
        if (same_text(drivers[i].name, name))
            return -1;
    drivers[driver_count++] = (struct driver_entry){name, class_code, subclass, prog_if, probe};
    return 0;
}

static void inspect_function(unsigned bus, unsigned slot, unsigned function) {
    uint32_t id = pci_config_read(bus, slot, function, 0);
    uint16_t vendor = (uint16_t)id;
    if (vendor == 0xffff)
        return;
    uint32_t header = pci_config_read(bus, slot, function, 0x0c);
    if ((uint8_t)(header >> 16) == 1) {

        uint32_t command = pci_config_read(bus, slot, function, 4);
        pci_config_write(bus, slot, function, 4,
                         command | KNX_PCI_ENABLE_MEMORY | KNX_PCI_ENABLE_BUS_MASTER);
    }
    uint32_t class_info = pci_config_read(bus, slot, function, 8);
    knx_pci_device pci = {0};
    pci.bus = (uint8_t)bus;
    pci.device = (uint8_t)slot;
    pci.function = (uint8_t)function;
    pci.vendor_id = vendor;
    pci.device_id = (uint16_t)(id >> 16);
    pci.class_code = (uint8_t)(class_info >> 24);
    pci.subclass = (uint8_t)(class_info >> 16);
    pci.prog_if = (uint8_t)(class_info >> 8);
    pci.header_type = (uint8_t)(header >> 16);
    if ((pci.header_type & 0x7f) == 0)
        for (unsigned i = 0; i < 6; ++i)
            pci.bar[i] = pci_config_read(bus, slot, function, 0x10 + i * 4);
    uint32_t interrupt = pci_config_read(bus, slot, function, 0x3c);
    pci.interrupt_line = (uint8_t)interrupt;
    pci.interrupt_pin = (uint8_t)(interrupt >> 8);

    const char *bound = "unbound";
    for (unsigned i = 0; i < driver_count; ++i) {
        struct driver_entry *d = &drivers[i];
        if ((d->class_code == 0xff || d->class_code == pci.class_code) &&
            (d->subclass == 0xff || d->subclass == pci.subclass) &&
            (d->prog_if == 0xff || d->prog_if == pci.prog_if) && d->probe(&pci)) {
            bound = d->name;
            ++matched_count;
            break;
        }
    }
    if (device_count < MAX_DEVICES)
        devices[device_count++] = (struct device_entry){pci, bound};
}

void knx_devices_init(void) {
    device_count = matched_count = 0;
    if (!builtin_drivers_registered) {
        if (knx_driver_register("xHCI USB host", 0x0c, 0x03, 0x30, knx_xhci_probe) == 0)
            builtin_drivers_registered = 1;
        else
            knx_log("[DRIVER]", "Could not register the built-in xHCI driver");

        knx_driver_register("EHCI USB 2.0 (companion handoff)", 0x0c, 0x03, 0x02,
                            knx_ehci_probe);

        knx_driver_register("OHCI USB 1.1 host", 0x0c, 0x03, 0xff, knx_ohci_probe);
        knx_driver_register("UHCI USB 1.1 host", 0x0c, 0x03, 0x00, knx_uhci_probe);

        knx_driver_register("Intel LPSS I2C", 0x0c, 0x80, 0x00, knx_i2c_lpss_probe);

        knx_driver_register("RTL8139 Ethernet", 0x02, 0x00, 0xff, knx_rtl8139_probe);
        knx_driver_register("Intel PRO/1000 Ethernet", 0x02, 0x00, 0xff, knx_e1000_probe);

        knx_driver_register("ES1370 AC97 audio", 0x04, 0x01, 0xff, knx_es1370_probe);
        knx_driver_register("HDA Azalia audio", 0x04, 0x03, 0x00, knx_hda_probe);
    }
    for (unsigned bus = 0; bus < 256; ++bus) {
        for (unsigned slot = 0; slot < 32; ++slot) {
            if ((uint16_t)pci_config_read(bus, slot, 0, 0) == 0xffff)
                continue;
            uint8_t header = (uint8_t)(pci_config_read(bus, slot, 0, 0x0c) >> 16);
            unsigned functions = (header & 0x80) ? 8 : 1;
            for (unsigned function = 0; function < functions; ++function)
                inspect_function(bus, slot, function);
        }
    }
    knx_puts("Devices: ");
    knx_print_u64(device_count);
    knx_puts(" PCI functions, ");
    knx_print_u64(matched_count);
    knx_puts(" bound drivers.\n");
}

void knx_devices_info(void) {
    knx_puts("PCI devices: ");
    knx_print_u64(device_count);
    knx_puts("; bound: ");
    knx_print_u64(matched_count);
    knx_puts("\n");
    for (unsigned i = 0; i < device_count; ++i) {
        const struct device_entry *e = &devices[i];
        knx_puts("  ");
        knx_print_u64(e->pci.bus);
        knx_putc(':');
        knx_print_u64(e->pci.device);
        knx_putc('.');
        knx_print_u64(e->pci.function);
        knx_puts(" class ");
        knx_print_u64(e->pci.class_code);
        knx_putc(':');
        knx_print_u64(e->pci.subclass);
        knx_putc(':');
        knx_print_u64(e->pci.prog_if);
        knx_puts(" id=");
        knx_print_u64(e->pci.vendor_id);
        knx_putc(':');
        knx_print_u64(e->pci.device_id);
        knx_puts(" BAR0=");
        knx_print_u64(e->pci.bar[0]);
        knx_puts(" IRQ=");
        if (e->pci.interrupt_pin) {
            knx_putc((char)('A' + e->pci.interrupt_pin - 1));
            knx_putc(':');
            knx_print_u64(e->pci.interrupt_line);
        } else {
            knx_puts("none");
        }
        knx_puts(" driver=");
        knx_puts(e->driver);
        knx_puts("\n");
    }
}
