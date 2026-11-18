#include "knx.h"

#define MAX_DEVICES 64

/* QEMU "virt" maps the PCIe enhanced configuration access mechanism here. */
#define KNX_PCI_ECAM_BASE 0x4010000000ULL

struct device_entry {
    knx_pci_device pci;
    const char *driver;
};
static struct device_entry devices[MAX_DEVICES];
static unsigned device_count, matched_count;
static int builtin_drivers_registered;

/* Implemented in driver.c (the PCI driver registry owns its own entry type). */
extern int knx_driver_match(const knx_pci_device *pci, const char **name_out);

static volatile uint32_t *ecam_reg(unsigned bus, unsigned slot, unsigned function,
                                   unsigned offset) {
    uint64_t address = KNX_PCI_ECAM_BASE |
                       ((uint64_t)bus << 20) | ((uint64_t)slot << 15) |
                       ((uint64_t)function << 12) | ((uint64_t)(offset & 0xfffu));
    return (volatile uint32_t *)(uintptr_t)address;
}

static uint32_t pci_config_read(unsigned bus, unsigned slot, unsigned function, unsigned offset) {
    uint32_t value = *ecam_reg(bus, slot, function, offset);
    knx_dmb();
    return value;
}

static void pci_config_write(unsigned bus, unsigned slot, unsigned function, unsigned offset,
                             uint32_t value) {
    *ecam_reg(bus, slot, function, offset) = value;
    knx_dmb();
}

uint32_t knx_pci_read32(const knx_pci_device *device, unsigned offset) {
    if (!device || offset >= 256)
        return 0xffffffffu;
    return pci_config_read(device->bus, device->device, device->function, offset);
}

void knx_pci_write32(const knx_pci_device *device, unsigned offset, uint32_t value) {
    if (!device || offset >= 256)
        return;
    pci_config_write(device->bus, device->device, device->function, offset, value);
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
    if (knx_driver_match(&pci, &bound))
        ++matched_count;
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

        /* Legacy USB, ISA audio, Intel LPSS I2C and RTL8139 are not present
         * on the target platform.  xHCI and e1000 are MMIO PCIe devices and
         * remain supported. */
        knx_driver_register("Intel PRO/1000 Ethernet (MMIO)", 0x02, 0x00, 0xff, knx_e1000_probe);
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
        knx_puts(" BAR0=0x");
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
