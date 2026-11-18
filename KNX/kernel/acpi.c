#include "knx.h"

/*
 * ACPI table discovery is retained (the boot loader hands us the RSDP and the
 * tables can be walked directly).  Power control, however, no longer uses GAS
 * I/O-port accesses: AArch64 systems are reset/powered off through PSCI, which
 * the firmware already implements (QEMU virt uses PSCI via HVC).
 */

struct __attribute__((packed)) acpi_header {
    char signature[4];
    uint32_t length;
    uint8_t revision, checksum;
    uint8_t oem[6], table_id[8];
    uint32_t oem_revision, creator_id, creator_revision;
};

static volatile struct acpi_header *fadt;
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

    /* The sleep control fields are parsed for informational purposes only;
     * poweroff/reboot are handled by PSCI below. */
    volatile uint8_t *p = (volatile uint8_t *)fadt;
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
    if (have_sleep_type) {
        knx_puts("; S5 poweroff type A=");
        knx_print_u64(sleep_type_a);
        knx_puts(" B=");
        knx_print_u64(sleep_type_b);
        knx_puts("\n");
    } else {
        knx_puts("; S5 poweroff unavailable\n");
    }
    knx_puts("Power control: PSCI (hvc #0, SYSTEM_OFF/SYSTEM_RESET)\n");
}

/* PSCI 32-bit calls: x0 = function id, result in x0 (0 = success).  The
 * SYSTEM_OFF and SYSTEM_RESET calls do not return when the firmware accepts
 * them; a returned value is therefore a failure status. */
static inline uint64_t psci_call(uint64_t fid) {
    register uint64_t x0 __asm__("x0") = fid;
    __asm__ volatile("hvc #0" : "+r"(x0) : : "memory");
    return x0;
}

#define PSCI_SYSTEM_OFF  0x84000008ULL
#define PSCI_SYSTEM_RESET 0x84000009ULL

/* Return nonzero when the firmware rejected the request. */
int knx_acpi_reboot(void) {
    return psci_call(PSCI_SYSTEM_RESET) != 0;
}

int knx_acpi_poweroff(void) {
    return psci_call(PSCI_SYSTEM_OFF) != 0;
}
