#include "knx.h"

/*
 * AArch64 secondary-CPU entry support and stubs for peripherals that are not
 * present on the target platform.  The PSCI CPU_ON call in smp.c passes a
 * mailbox pointer in x2; kernel/entry.S sets SP from it and calls
 * knx_arch_secondary_init() with the CPU index in x0.
 */

void knx_arch_secondary_init(uint64_t cpu_index) {
    uint64_t root = knx_vm_current_root();

    __asm__ volatile("msr mair_el1, %0" ::"r"(0xff00ULL) : "memory");

    uint64_t tcr = (16ULL << 0) | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) |
                   (1ULL << 23) | (5ULL << 32);
    __asm__ volatile("msr tcr_el1, %0" ::"r"(tcr) : "memory");
    __asm__ volatile("msr ttbr0_el1, %0" ::"r"(root) : "memory");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 12);
    __asm__ volatile("msr sctlr_el1, %0" ::"r"(sctlr) : "memory");
    __asm__ volatile("dsb sy\n\ttlbi vmalle1\n\tdsb sy\n\tisb" ::: "memory");

    knx_ap_main(cpu_index);
}

/* ISA audio, Intel LPSS I2C and the PC speaker are not present on the
 * target platform, so these entry points report "device absent". */
void knx_audio_tick(void) {}
void knx_i2c_tick(void) {}
int knx_pcm_write(const uint8_t *buf, uint64_t len) {
    (void)buf;
    (void)len;
    return -19; /* ENXIO: no PCM backend on this platform */
}
