#include "knx.h"

/*
 * AArch64 SMP via PSCI.
 *
 * Boot CPU affinity is taken from MPIDR_EL1 and secondary CPUs are released
 * with PSCI CPU_ON.  Each secondary receives a small mailbox (as its PSCI
 * context ID)
 * carrying its kernel stack and index.  Anything that fails leaves the kernel
 * running single-CPU.
 */

#define KNX_MAX_CPUS 16u
#define PSCI_CPU_ON 0xc4000003ULL

extern void knx_secondary_entry(void); /* provided by kernel/entry.S */

struct secondary_mailbox {
    volatile uint64_t stack_top;
    volatile uint64_t cpu_index;
    volatile uint64_t entry_ready;
};

static struct secondary_mailbox mailboxes[KNX_MAX_CPUS] __attribute__((aligned(64)));
static uint64_t secondary_stacks[KNX_MAX_CPUS - 1u][512] __attribute__((aligned(16)));
static volatile uint8_t cpu_online[KNX_MAX_CPUS];
static volatile uint64_t cpu_count;
static uint64_t bsp_mpidr;
static unsigned bsp_aff0;

/* PSCI call: x0 = function id, x1..x3 = arguments, result returned in x0. */
static inline int64_t psci_call(uint64_t fid, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    register uint64_t x0 __asm__("x0") = fid;
    register uint64_t x1 __asm__("x1") = arg0;
    register uint64_t x2 __asm__("x2") = arg1;
    register uint64_t x3 __asm__("x3") = arg2;
    __asm__ volatile("hvc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return (int64_t)x0;
}

static inline uint64_t read_mpidr(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr;
}

void knx_smp_init(knx_boot_info *info) {
    (void)info;

    bsp_mpidr = read_mpidr();
    bsp_aff0 = (unsigned)(bsp_mpidr & 0xffu);
    if (bsp_aff0 >= KNX_MAX_CPUS)
        bsp_aff0 = 0;

    cpu_count = 1;
    cpu_online[0] = 1;
    knx_task_cpu_online(0);

    for (unsigned cpu = 1u; cpu < KNX_MAX_CPUS; ++cpu) {
        struct secondary_mailbox *mb = &mailboxes[cpu];
        if (cpu == bsp_aff0)
            continue;

        mb->stack_top = (uint64_t)(uintptr_t)&secondary_stacks[cpu - 1u][512];
        mb->cpu_index = cpu;
        mb->entry_ready = 0;
        knx_dmb();

        /* MPIDR Aff0 == cpu (Aff1..Aff3 == 0 on QEMU virt and most platforms). */
        uint64_t target_mpidr = (uint64_t)cpu;
        int64_t result = psci_call(PSCI_CPU_ON, target_mpidr,
                                   (uint64_t)(uintptr_t)&knx_secondary_entry,
                                   (uint64_t)(uintptr_t)mb);
        if (result != 0) /* PSCI_SUCCESS == 0; everything else: stay single-CPU. */
            continue;

        /* Give the secondary a brief bounded window to announce itself. */
        uint64_t deadline = knx_read_tsc() + knx_tsc_hz() / 1000u;
        while (!mb->entry_ready && knx_read_tsc() < deadline)
            knx_pause();

        cpu_online[cpu] = 1;
        ++cpu_count;
        knx_task_cpu_online(cpu);
    }
}

void knx_ap_main(uint64_t cpu_index) {
    if (cpu_index == 0u || cpu_index >= KNX_MAX_CPUS)
        for (;;)
            knx_halt();

    knx_percpu_init_cpu((unsigned)cpu_index);
    knx_set_kernel_stack(mailboxes[cpu_index].stack_top);
    mailboxes[cpu_index].entry_ready = 1;
    knx_dmb();

    for (;;)
        knx_halt();
}

uint64_t knx_smp_cpu_count(void) {
    return cpu_count;
}

uint64_t knx_smp_cpu_index(void) {
    uint64_t aff0 = read_mpidr() & 0xffu;
    return aff0 > (KNX_MAX_CPUS - 1u) ? (KNX_MAX_CPUS - 1u) : aff0;
}

int knx_smp_is_bsp(void) {
    return knx_smp_cpu_index() == 0u;
}

/* The arch timer is a per-CPU PPI; the GIC CPU interface is enabled per CPU
 * by the secondary entry path, so these are intentionally no-ops. */
void knx_smp_ap_timer_init(void) {
}

void knx_smp_ap_timer_eoi(void) {
}

void knx_smp_info(void) {
    knx_puts("SMP: PSCI, online CPUs: ");
    knx_print_u64(cpu_count);
    knx_puts(", current CPU index: ");
    knx_print_u64(knx_smp_cpu_index());
    knx_puts(", BSP MPIDR: 0x");
    knx_print_u64(bsp_mpidr);
    knx_puts("\nCPU status:");
    for (unsigned cpu = 0; cpu < KNX_MAX_CPUS; ++cpu) {
        if (!cpu_online[cpu])
            continue;
        knx_puts(" ");
        knx_print_u64(cpu);
        knx_puts(cpu == 0u ? "=BSP" : "=online");
    }
    knx_puts("\n");
}
