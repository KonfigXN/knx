#include "knx.h"

#define MAX_CPU_CONTEXTS 16
struct __attribute__((packed)) descriptor_ptr {
    uint16_t limit;
    uint64_t base;
};
struct __attribute__((packed)) tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3, iomap;
};
static struct tss64 cpu_tss[MAX_CPU_CONTEXTS] __attribute__((aligned(16)));
static uint64_t cpu_gdt[MAX_CPU_CONTEXTS][7] __attribute__((aligned(16)));
extern void knx_load_gdt(const struct descriptor_ptr *);
extern void knx_load_tr(void);

void knx_gdt_init_cpu(unsigned cpu) {
    if (cpu >= MAX_CPU_CONTEXTS)
        cpu = 0;
    volatile uint64_t *gdt = cpu_gdt[cpu];
    gdt[0] = 0;
    gdt[1] = 0x00af9a000000ffffULL;
    gdt[2] = 0x00cf92000000ffffULL;
    gdt[3] = 0x00affa000000ffffULL;
    gdt[4] = 0x00cff2000000ffffULL;
    uint64_t base = (uint64_t)(uintptr_t)&cpu_tss[cpu];
    uint64_t limit = sizeof(cpu_tss[cpu]) - 1;
    gdt[5] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ULL << 40) |
             ((limit & 0xf0000) << 32) | ((base & 0xff000000) << 32);
    gdt[6] = base >> 32;
    cpu_tss[cpu].iomap = sizeof(cpu_tss[cpu]);
    struct descriptor_ptr p = {sizeof(cpu_gdt[cpu]) - 1, (uint64_t)(uintptr_t)cpu_gdt[cpu]};
    knx_load_gdt(&p);
    knx_load_tr();
}

void knx_gdt_init(void) {
    knx_gdt_init_cpu(0);
}

void knx_set_kernel_stack(uint64_t stack_top) {
    unsigned cpu = (unsigned)knx_smp_cpu_index();
    if (cpu < MAX_CPU_CONTEXTS)
        cpu_tss[cpu].rsp0 = stack_top;
}

uint64_t knx_kernel_stack_top(void) {
    unsigned cpu = (unsigned)knx_smp_cpu_index();
    if (cpu >= MAX_CPU_CONTEXTS)
        cpu = 0;
    return cpu_tss[cpu].rsp0;
}
