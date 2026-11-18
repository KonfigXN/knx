#include "knx.h"

/*
 * AArch64 per-CPU kernel-stack layer built on TPIDR_EL1.
 *
 * TPIDR_EL1 points at the current CPU's small context structure, which holds
 * the top of that CPU's kernel exception stack.  The exported names are kept
 * because the rest of the kernel still calls them.
 */

#define MAX_CPU_CONTEXTS 16

struct cpu_context {
    uint64_t stack_top;
};

static struct cpu_context contexts[MAX_CPU_CONTEXTS] __attribute__((aligned(64)));

static inline void write_tpidr_el1(uint64_t value) {
    __asm__ volatile("msr tpidr_el1, %0" ::"r"(value) : "memory");
}

static inline uint64_t read_tpidr_el1(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(value));
    return value;
}

/* Prefer the TPIDR_EL1 pointer; fall back to the MPIDR-derived index when the
 * register has not been set up yet (e.g. early on an AP). */
static struct cpu_context *current_context(void) {
    uint64_t ptr = read_tpidr_el1();
    uintptr_t low = (uintptr_t)&contexts[0];
    uintptr_t high = (uintptr_t)&contexts[0] + sizeof(contexts);
    if (ptr >= (uint64_t)low && ptr < (uint64_t)high)
        return (struct cpu_context *)(uintptr_t)ptr;

    unsigned cpu = (unsigned)knx_smp_cpu_index();
    if (cpu >= MAX_CPU_CONTEXTS)
        cpu = 0;
    return &contexts[cpu];
}

void knx_percpu_init_cpu(unsigned cpu) {
    if (cpu >= MAX_CPU_CONTEXTS)
        cpu = 0;
    write_tpidr_el1((uint64_t)(uintptr_t)&contexts[cpu]);
}

void knx_percpu_init(void) {
    knx_percpu_init_cpu(0);
}

void knx_set_kernel_stack(uint64_t stack_top) {
    current_context()->stack_top = stack_top;
}

uint64_t knx_kernel_stack_top(void) {
    return current_context()->stack_top;
}
