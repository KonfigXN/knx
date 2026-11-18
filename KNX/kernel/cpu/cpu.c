#include "knx.h"
static char vendor[13] = "Unknown";
static char brand[49] = "Unknown x86_64 CPU";
static uint32_t feature_ecx, feature_edx, ext_edx;
static uint32_t logical_per_package = 1, cores_per_package = 1, threads_per_core = 1;
static uint32_t cpuid_logical_count = 1, cpuid_bsp_id;

static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b, uint32_t *c,
                  uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(subleaf));
}

void knx_cpu_init(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    *(uint32_t *)&vendor[0] = b;
    *(uint32_t *)&vendor[4] = d;
    *(uint32_t *)&vendor[8] = c;
    vendor[12] = 0;
    uint32_t max = a;
    if (max >= 1) {
        cpuid(1, 0, &a, &b, &feature_ecx, &feature_edx);
        cpuid_logical_count = (b >> 16) & 255;
        if (!cpuid_logical_count)
            cpuid_logical_count = 1;
        cpuid_bsp_id = b >> 24;
    }

    uint32_t topology_leaf = max >= 0x1f ? 0x1f : (max >= 0x0b ? 0x0b : 0);
    if (topology_leaf) {
        uint32_t smt_count = 0, package_count = 0;
        for (uint32_t level = 0; level < 8; ++level) {
            cpuid(topology_leaf, level, &a, &b, &c, &d);
            uint32_t kind = (c >> 8) & 255;
            if (!b || !kind)
                break;
            if (kind == 1)
                smt_count = b;
            if (kind == 2)
                package_count = b;
        }
        if (package_count) {
            logical_per_package = package_count;
            threads_per_core = smt_count ? smt_count : 1;
            cores_per_package = package_count / threads_per_core;
            if (!cores_per_package)
                cores_per_package = 1;
        }
    } else if (max >= 4) {
        cpuid(4, 0, &a, &b, &c, &d);
        if ((a & 31) != 0)
            cores_per_package = ((a >> 26) & 63) + 1;
        threads_per_core = cpuid_logical_count / cores_per_package;
        if (!threads_per_core)
            threads_per_core = 1;
        logical_per_package = cpuid_logical_count;
    }
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    uint32_t max_extended = a;
    if (max_extended >= 0x80000004) {
        uint32_t *p = (uint32_t *)brand;
        for (uint32_t leaf = 0; leaf < 3; ++leaf) {
            a = 0x80000002 + leaf;
            __asm__ volatile("cpuid"
                             : "=a"(p[0]), "=b"(p[1]), "=c"(p[2]), "=d"(p[3])
                             : "a"(a), "c"(0));
            p += 4;
        }
        brand[48] = 0;
    }
    if (max_extended >= 0x80000001)
        cpuid(0x80000001, 0, &a, &b, &c, &ext_edx);
}
void knx_cpu_info(void) {
    knx_puts("CPU vendor: ");
    knx_puts(vendor);
    knx_puts("\nCPU model: ");
    knx_puts(brand);
    knx_puts("\nFeatures: ");
    knx_puts((ext_edx & (1u << 29)) ? "x86_64 " : "x86_64-unsupported ");
    knx_puts((feature_edx & (1u << 4)) ? "TSC " : "");
    knx_puts((feature_edx & (1u << 26)) ? "SSE2 " : "");
    knx_puts((feature_ecx & (1u << 25)) ? "AES " : "");
    knx_puts((feature_ecx & (1u << 21)) ? "x2APIC " : "");
    knx_puts((feature_ecx & (1u << 28)) ? "AVX " : "");
    knx_puts("\n");
}

void knx_cpu_topology(void) {
    knx_puts("CPUID topology: ");
    knx_print_u64(logical_per_package);
    knx_puts(" logical/package, ");
    knx_print_u64(cores_per_package);
    knx_puts(" cores/package, ");
    knx_print_u64(threads_per_core);
    knx_puts(" threads/core; CPUID logical count: ");
    knx_print_u64(cpuid_logical_count);
    knx_puts("; CPUID BSP APIC ID: ");
    knx_print_u64(cpuid_bsp_id);
    knx_puts("\n");
}
