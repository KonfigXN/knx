#pragma once

/*
 * KNX AArch64 architecture layer.
 *
 * This header is the single place where the rest of the kernel gets its
 * AArch64 architecture primitives:
 *
 *   pause()          -> yield
 *   interrupt mask   -> DAIF.IRQ
 *   idle             -> wfi
 *   timestamp        -> cntvct_el0
 *   syscall entry    -> svc #0
 *   page-table root  -> TTBR0_EL1
 *
 * Kernel code uses the helpers and constants below instead of raw system
 * registers so the rest of the tree stays architecture-neutral.
 */

#define KNX_ARCH_AARCH64 1

/* ELF constants used by the VM loader and the extension loader. */
#define KNX_ELF_MACHINE 183 /* EM_AARCH64 */
#define KNX_R_ABS64 257     /* R_AARCH64_ABS64 */
#define KNX_R_ABS32 258     /* R_AARCH64_ABS32 */
#define KNX_R_CALL26 283    /* R_AARCH64_CALL26 */
#define KNX_R_JUMP26 282    /* R_AARCH64_JUMP26 */
#define KNX_R_ADRP_HI21 275 /* R_AARCH64_ADR_PREL_PG_HI21 */
#define KNX_R_ADD_LO12 285  /* R_AARCH64_ADD_ABS_LO12_NC */
#define KNX_R_RELATIVE 1027 /* R_AARCH64_RELATIVE */

/* A task/exception frame is 40 words.  These indices are the shared ABI
 * between the assembly vectors, the scheduler and the syscall dispatcher.
 * See kernel/interrupts.S for the assembly side. */
#define KNX_FRAME_WORDS 40u
#define KNX_FRAME_X19 0u
#define KNX_FRAME_X20 1u
#define KNX_FRAME_X21 2u
#define KNX_FRAME_X22 3u
#define KNX_FRAME_X23 4u
#define KNX_FRAME_X24 5u
#define KNX_FRAME_ARG5 6u  /* x5 */
#define KNX_FRAME_ARG4 7u  /* x4 */
#define KNX_FRAME_ARG1 8u  /* x1 */
#define KNX_FRAME_ARG0 9u  /* x0 */
#define KNX_FRAME_X25 10u
#define KNX_FRAME_ARG2 11u /* x2 */
#define KNX_FRAME_ARG3 12u /* x3 */
#define KNX_FRAME_X26 13u
#define KNX_FRAME_NUMBER 14u /* x8: syscall number / return value */
#define KNX_FRAME_VECTOR 15u
#define KNX_FRAME_ERROR 16u
#define KNX_FRAME_PC 17u    /* elr_el1 */
#define KNX_FRAME_CS 18u    /* synthetic: 3 = from EL0, 0 = from EL1 */
#define KNX_FRAME_FLAGS 19u /* spsr_el1 */
#define KNX_FRAME_SP 20u
#define KNX_FRAME_SS 21u
#define KNX_FRAME_X27 22u
#define KNX_FRAME_X28 23u
#define KNX_FRAME_X29 24u
#define KNX_FRAME_X30 25u
#define KNX_FRAME_X6 26u
#define KNX_FRAME_X7 27u
#define KNX_FRAME_X9 28u
#define KNX_FRAME_X10 29u
#define KNX_FRAME_X11 30u
#define KNX_FRAME_X12 31u
#define KNX_FRAME_X13 32u
#define KNX_FRAME_X14 33u
#define KNX_FRAME_X15 34u
#define KNX_FRAME_X16 35u
#define KNX_FRAME_X17 36u
#define KNX_FRAME_X18 37u
#define KNX_FRAME_FAR 38u
#define KNX_FRAME_RESERVED 39u

#ifndef __ASSEMBLER__

#include <stdint.h>

static inline void knx_pause(void) { __asm__ volatile("yield" ::: "memory"); }
static inline void knx_dmb(void) { __asm__ volatile("dmb sy" ::: "memory"); }
static inline void knx_dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void knx_isb(void) { __asm__ volatile("isb" ::: "memory"); }
static inline void knx_halt(void) { __asm__ volatile("wfi" ::: "memory"); }

static inline uint64_t knx_irq_flags_save(void) {
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2" ::: "memory");
    return daif;
}

static inline void knx_irq_flags_restore(uint64_t daif) {
    __asm__ volatile("msr daif, %0" ::"r"(daif) : "memory");
}

static inline void knx_irq_disable(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
static inline void knx_irq_enable(void) { __asm__ volatile("msr daifclr, #2" ::: "memory"); }

static inline uint64_t knx_read_tsc(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

static inline uint64_t knx_tsc_hz(void) {
    uint64_t hz;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    return hz ? hz : 1000000000ULL;
}

static inline uint64_t knx_read_sp(void) {
    uint64_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    return sp;
}

#define KNX_ARCH_TIMER_IRQ 30u /* PPI 30: CNTV (secure physical) */

#endif /* __ASSEMBLER__ */
