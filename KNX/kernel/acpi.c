#include "knx.h"

struct __attribute__((packed)) acpi_header {
    char signature[4];
    uint32_t length;
    uint8_t revision, checksum;
    uint8_t oem[6], table_id[8];
    uint32_t oem_revision, creator_id, creator_revision;
};
struct gas {
    uint8_t space, width, offset, access_size;
    uint64_t address;
};

static volatile struct acpi_header *fadt;
static uint32_t pm1a_port, pm1b_port, smi_command;
static uint8_t acpi_enable_value, pm1_control_length;
static struct gas pm1a_gas, pm1b_gas, reset_gas;
static uint8_t reset_value;
static uint8_t sleep_type_a, sleep_type_b;
static int have_sleep_type;

static uint8_t checksum(const volatile uint8_t *p, uint32_t length) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; ++i)
        sum = (uint8_t)(sum + p[i]);
    return sum;
}

static uint32_t read32(const volatile uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t read64(const volatile uint8_t *p) {
    return (uint64_t)read32(p) | (uint64_t)read32(p + 4) << 32;
}

static volatile uint8_t *find_rsdp(knx_boot_info *info) {
    if (info && info->acpi_rsdp) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)info->acpi_rsdp;
        static const char sig[] = "RSD PTR ";
        unsigned i = 0;
        while (i < 8 && p[i] == (uint8_t)sig[i])
            ++i;
        if (i == 8 && checksum(p, 20) == 0)
            return p;
    }
    for (uint32_t address = 0xe0000; address < 0x100000; address += 16) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)address;
        static const char sig[] = "RSD PTR ";
        unsigned i = 0;
        while (i < 8 && p[i] == (uint8_t)sig[i])
            ++i;
        if (i == 8 && checksum(p, 20) == 0)
            return p;
    }
    return 0;
}

static volatile struct acpi_header *find_table(uint64_t root_address, int xsdt,
                                               const char signature[4]) {
    volatile struct acpi_header *root = (volatile struct acpi_header *)(uintptr_t)root_address;
    if (!root || root->length < sizeof(*root) || root->length > 0x100000 ||
        checksum((volatile uint8_t *)root, root->length) != 0)
        return 0;
    uint32_t entry_size = xsdt ? 8 : 4;
    uint32_t count = (root->length - sizeof(*root)) / entry_size;
    volatile uint8_t *entries = (volatile uint8_t *)root + sizeof(*root);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t address = xsdt ? read64(entries + i * 8) : read32(entries + i * 4);
        volatile struct acpi_header *h = (volatile struct acpi_header *)(uintptr_t)address;
        if (h && h->length >= sizeof(*h) && h->length <= 0x100000 &&
            h->signature[0] == signature[0] && h->signature[1] == signature[1] &&
            h->signature[2] == signature[2] && h->signature[3] == signature[3] &&
            checksum((volatile uint8_t *)h, h->length) == 0)
            return h;
    }
    return 0;
}

static uint64_t aml_integer(const volatile uint8_t *p, uint32_t end, uint32_t *used) {
    if (!p || !used || !end)
        return UINT64_MAX;
    switch (p[0]) {
    case 0x00:
        *used = 1;
        return 0;
    case 0x01:
        *used = 1;
        return 1;
    case 0xff:
        *used = 1;
        return UINT64_MAX - 1;
    case 0x0a:
        if (end < 2)
            return UINT64_MAX;
        *used = 2;
        return p[1];
    case 0x0b:
        if (end < 3)
            return UINT64_MAX;
        *used = 3;
        return (uint64_t)p[1] | (uint64_t)p[2] << 8;
    case 0x0c:
        if (end < 5)
            return UINT64_MAX;
        *used = 5;
        return read32(p + 1);
    case 0x0e:
        if (end < 9)
            return UINT64_MAX;
        *used = 9;
        return read64(p + 1);
    default:
        return UINT64_MAX;
    }
}

static int parse_s5(volatile struct acpi_header *dsdt) {
    if (!dsdt || dsdt->length <= sizeof(*dsdt))
        return 0;
    volatile uint8_t *aml = (volatile uint8_t *)dsdt + sizeof(*dsdt);
    uint32_t length = dsdt->length - sizeof(*dsdt);
    for (uint32_t i = 0; i + 8 < length; ++i) {
        if (aml[i] != '_' || aml[i + 1] != 'S' || aml[i + 2] != '5' || aml[i + 3] != '_')
            continue;
        uint32_t p = i + 4;
        if (p >= length || aml[p++] != 0x12)
            continue;
        if (p >= length)
            continue;
        uint8_t lead = aml[p];
        uint32_t pkg_bytes = (lead >> 6) + 1;
        if (p + pkg_bytes >= length)
            continue;
        p += pkg_bytes;
        uint8_t elements = aml[p++];
        if (elements < 2 || p >= length)
            continue;
        uint32_t used_a = 0, used_b = 0;
        uint64_t a = aml_integer(aml + p, length - p, &used_a);
        if (a == UINT64_MAX || p + used_a >= length)
            continue;
        p += used_a;
        uint64_t b = aml_integer(aml + p, length - p, &used_b);
        if (b == UINT64_MAX)
            continue;
        sleep_type_a = (uint8_t)(a & 7);
        sleep_type_b = (uint8_t)(b & 7);
        have_sleep_type = 1;
        return 1;
    }
    return 0;
}

static struct gas load_gas(const volatile uint8_t *p) {
    struct gas g;
    g.space = p[0];
    g.width = p[1];
    g.offset = p[2];
    g.access_size = p[3];
    g.address = read64(p + 4);
    return g;
}

void knx_acpi_init(knx_boot_info *info) {
    volatile uint8_t *rsdp = find_rsdp(info);
    if (!rsdp)
        return;
    uint64_t root = read32(rsdp + 16);
    int xsdt = 0;
    uint32_t rsdp_length = read32(rsdp + 20);
    if (rsdp[15] >= 2 && rsdp_length >= 36 && rsdp_length <= 4096 &&
        checksum(rsdp, rsdp_length) == 0) {
        uint64_t xroot = read64(rsdp + 24);
        if (xroot) {
            root = xroot;
            xsdt = 1;
        }
    }
    fadt = find_table(root, xsdt, "FACP");
    if (!fadt || fadt->length < 90)
        return;
    volatile uint8_t *p = (volatile uint8_t *)fadt;
    pm1a_port = read32(p + 64);
    pm1b_port = read32(p + 68);
    smi_command = read32(p + 48);
    acpi_enable_value = p[52];
    pm1_control_length = p[89];
    if (fadt->length >= 196) {
        pm1a_gas = load_gas(p + 172);
        pm1b_gas = load_gas(p + 184);
    }
    if (fadt->length >= 129) {
        reset_gas = load_gas(p + 116);
        reset_value = p[128];
    }
    uint64_t dsdt_address = read32(p + 40);
    if (fadt->length >= 148 && read64(p + 140))
        dsdt_address = read64(p + 140);
    volatile struct acpi_header *dsdt = (volatile struct acpi_header *)(uintptr_t)dsdt_address;
    if (dsdt && dsdt->length >= sizeof(*dsdt) && dsdt->length <= 0x100000 &&
        checksum((volatile uint8_t *)dsdt, dsdt->length) == 0)
        (void)parse_s5(dsdt);
}

void knx_acpi_info(void) {
    knx_puts("ACPI: ");
    knx_puts(fadt ? "FADT available" : "power-management tables unavailable");
    knx_puts(have_sleep_type ? "; S5 poweroff available\n" : "; S5 poweroff unavailable\n");
}

static int gas_write(const struct gas *g, uint8_t value) {
    if (!g->address)
        return 0;
    if (g->space == 1 && g->address <= 0xffff) {
        __asm__ volatile("outb %0,%1" : : "a"(value), "Nd"((uint16_t)g->address));
        return 1;
    }
    if (g->space == 0) {
        *(volatile uint8_t *)(uintptr_t)g->address = value;
        return 1;
    }
    return 0;
}

static int gas_write16(const struct gas *g, uint16_t value) {
    if (!g->address)
        return 0;
    if (g->space == 1 && g->address <= 0xfffe) {
        __asm__ volatile("outw %0,%1" : : "a"(value), "Nd"((uint16_t)g->address));
        return 1;
    }
    if (g->space == 0) {
        *(volatile uint16_t *)(uintptr_t)g->address = value;
        return 1;
    }
    return 0;
}

int knx_acpi_reboot(void) {
    return gas_write(&reset_gas, reset_value);
}

int knx_acpi_poweroff(void) {
    if (!fadt || !have_sleep_type || pm1_control_length < 2)
        return 0;
    if (smi_command && acpi_enable_value && pm1a_port && pm1a_port <= 0xffff) {
        uint16_t control;
        __asm__ volatile("inw %1,%0" : "=a"(control) : "Nd"((uint16_t)pm1a_port));
        if (!(control & 1)) {
            __asm__ volatile("outb %0,%1" : : "a"(acpi_enable_value), "Nd"((uint16_t)smi_command));
            for (unsigned i = 0; i < 1000000; ++i) {
                __asm__ volatile("inw %1,%0" : "=a"(control) : "Nd"((uint16_t)pm1a_port));
                if (control & 1)
                    break;
                __asm__ volatile("pause");
            }
        }
    }
    uint16_t value_a = (uint16_t)((sleep_type_a << 10) | (1u << 13));
    uint16_t value_b = (uint16_t)((sleep_type_b << 10) | (1u << 13));
    int wrote = 0;
    if (pm1a_gas.address)
        wrote |= gas_write16(&pm1a_gas, value_a);
    else if (pm1a_port && pm1a_port <= 0xffff) {
        __asm__ volatile("outw %0,%1" : : "a"(value_a), "Nd"((uint16_t)pm1a_port));
        wrote = 1;
    }
    if (pm1b_gas.address)
        wrote |= gas_write16(&pm1b_gas, value_b);
    else if (pm1b_port && pm1b_port <= 0xffff)
        __asm__ volatile("outw %0,%1" : : "a"(value_b), "Nd"((uint16_t)pm1b_port));
    return wrote;
}
