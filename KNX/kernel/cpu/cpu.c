#include "knx.h"

/*
 * AArch64 CPU identification.  CPUID is gone; everything comes from the
 * system registers MIDR_EL1, MPIDR_EL1, ID_AA64PFR0_EL1 and ID_AA64ISAR0_EL1.
 */

static char vendor[16] = "Unknown";
static char brand[64] = "Unknown AArch64 CPU";
static char features[128] = "";
static uint64_t midr_value;
static uint64_t mpidr_value;
static uint64_t pfr0_value;
static uint64_t isar0_value;

static void append_hex(char *dst, unsigned *pos, unsigned capacity, uint64_t value,
                       unsigned digits) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < digits && *pos + 1u < capacity; ++i) {
        unsigned shift = (digits - 1u - i) * 4u;
        dst[(*pos)++] = hex[(value >> shift) & 0xfu];
    }
    dst[*pos] = '\0';
}

static void append_str(char *dst, unsigned *pos, unsigned capacity, const char *text) {
    while (*text && *pos + 1u < capacity)
        dst[(*pos)++] = *text++;
    dst[*pos] = '\0';
}

void knx_cpu_init(void) {
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr_value));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr_value));
    __asm__ volatile("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0_value));
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0_value));

    unsigned implementer = (unsigned)((midr_value >> 24) & 0xffu);
    unsigned variant = (unsigned)((midr_value >> 20) & 0xfu);
    unsigned architecture = (unsigned)((midr_value >> 16) & 0xfu);
    unsigned partnum = (unsigned)((midr_value >> 4) & 0xfffu);
    unsigned revision = (unsigned)(midr_value & 0xfu);

    const char *vendor_name;
    switch (implementer) {
    case 0x41u: vendor_name = "ARM"; break;
    case 0x43u: vendor_name = "Cavium"; break;
    case 0x4du: vendor_name = "Freescale"; break;
    case 0x50u: vendor_name = "Qualcomm"; break;
    case 0x51u: vendor_name = "Qualcomm"; break;
    case 0x69u: vendor_name = "Intel"; break;
    case 0x7fu: vendor_name = "NVIDIA"; break;
    default:    vendor_name = "Unknown"; break;
    }
    unsigned pos = 0;
    append_str(vendor, &pos, sizeof(vendor), vendor_name);

    pos = 0;
    append_str(brand, &pos, sizeof(brand), "implementer 0x");
    append_hex(brand, &pos, sizeof(brand), implementer, 2);
    append_str(brand, &pos, sizeof(brand), " part 0x");
    append_hex(brand, &pos, sizeof(brand), partnum, 3);
    append_str(brand, &pos, sizeof(brand), " var 0x");
    append_hex(brand, &pos, sizeof(brand), variant, 1);
    append_str(brand, &pos, sizeof(brand), " arch 0x");
    append_hex(brand, &pos, sizeof(brand), architecture, 1);
    append_str(brand, &pos, sizeof(brand), " rev 0x");
    append_hex(brand, &pos, sizeof(brand), revision, 1);

    pos = 0;
    if (((pfr0_value >> 20) & 0xfu) == 0u)
        append_str(features, &pos, sizeof(features), "FP ");
    if (((pfr0_value >> 16) & 0xfu) == 0u)
        append_str(features, &pos, sizeof(features), "SIMD ");
    if (((pfr0_value >> 4) & 0xfu) >= 3u)
        append_str(features, &pos, sizeof(features), "GICv3 ");
    if (((isar0_value >> 4) & 0xfu) != 0u)
        append_str(features, &pos, sizeof(features), "AES ");
    if (((isar0_value >> 16) & 0xfu) != 0u)
        append_str(features, &pos, sizeof(features), "CRC32 ");
    if (((isar0_value >> 20) & 0xfu) != 0u)
        append_str(features, &pos, sizeof(features), "LSE ");
    if (!pos)
        append_str(features, &pos, sizeof(features), "none");
}

void knx_cpu_info(void) {
    knx_puts("CPU vendor: ");
    knx_puts(vendor);
    knx_puts("\nCPU model: ");
    knx_puts(brand);
    knx_puts("\nMIDR: 0x");
    knx_print_u64(midr_value);
    knx_puts("\nFeatures: ");
    knx_puts(features);
    knx_puts("\n");
}

void knx_cpu_topology(void) {
    knx_puts("MPIDR: 0x");
    knx_print_u64(mpidr_value);
    knx_puts("\nAffinity: ");
    knx_print_u64((mpidr_value >> 32) & 0xffu);
    knx_puts(".");
    knx_print_u64((mpidr_value >> 16) & 0xffu);
    knx_puts(".");
    knx_print_u64((mpidr_value >> 8) & 0xffu);
    knx_puts(".");
    knx_print_u64(mpidr_value & 0xffu);
    knx_puts(" (Aff3.Aff2.Aff1.Aff0)\n");
}
