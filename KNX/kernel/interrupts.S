/*
 * AArch64 exception vectors.
 *
 * VBAR_EL1 has sixteen 128-byte entries.  Each entry makes room for a 40-word
 * knx_frame, records the vector number, and branches to the common save path.
 * The common path stores every GPR plus ELR/SPSR/ESR/FAR, calls the C
 * dispatcher, then switches to the returned frame and returns with ERET.
 *
 * Frame indices are defined in include/knx_arch.h and shared with task.c.
 */
#define FRAME_SIZE 320

.section .text.vectors
.balign 0x800
.global knx_vector_table
knx_vector_table:

/* Helper: one VBAR entry.  x16 is preserved in the frame before it is used
 * as scratch, so no user register is lost. */
.macro KNX_VECTOR_ENTRY code
    .balign 0x80
    sub     sp, sp, #FRAME_SIZE         // allocate the exception frame
    str     xzr, [sp]
    str     x16, [sp, #(35*8)]          // save original x16
    mov     x16, #\code
    str     x16, [sp, #(15*8)]          // frame[15] = vector code
    b       knx_exception_enter
.endm

// Current EL, SP0
KNX_VECTOR_ENTRY 0                      // synchronous
KNX_VECTOR_ENTRY 1                      // IRQ
KNX_VECTOR_ENTRY 2                      // FIQ
KNX_VECTOR_ENTRY 3                      // SError
// Current EL, SPx (the kernel normally arrives here)
KNX_VECTOR_ENTRY 4                      // synchronous
KNX_VECTOR_ENTRY 5                      // IRQ
KNX_VECTOR_ENTRY 6                      // FIQ
KNX_VECTOR_ENTRY 7                      // SError
// Lower EL, AArch64 (user tasks)
KNX_VECTOR_ENTRY 8                      // synchronous: SVC / abort
KNX_VECTOR_ENTRY 9                      // IRQ
KNX_VECTOR_ENTRY 10                     // FIQ
KNX_VECTOR_ENTRY 11                     // SError
// Lower EL, AArch32 (unsupported; handled defensively)
KNX_VECTOR_ENTRY 12
KNX_VECTOR_ENTRY 13
KNX_VECTOR_ENTRY 14
KNX_VECTOR_ENTRY 15

.section .text
.global knx_exception_enter
.type knx_exception_enter,@function
knx_exception_enter:
    // General purpose registers not already stored by the vector entry.
    stp     x19, x20, [sp, #(0*8)]
    stp     x21, x22, [sp, #(2*8)]
    stp     x23, x24, [sp, #(4*8)]
    stp     x5,  x4,  [sp, #(6*8)]
    stp     x1,  x0,  [sp, #(8*8)]
    stp     x25, x2,  [sp, #(10*8)]
    stp     x3,  x26, [sp, #(12*8)]
    str     x8,       [sp, #(14*8)]
    stp     x17, x18, [sp, #(36*8)]
    stp     x27, x28, [sp, #(22*8)]
    stp     x29, x30, [sp, #(24*8)]
    stp     x6,  x7,  [sp, #(26*8)]
    stp     x9,  x10, [sp, #(28*8)]
    stp     x11, x12, [sp, #(30*8)]
    stp     x13, x14, [sp, #(32*8)]
    str     x15, [sp, #(34*8)]          // x16 was saved by the vector entry

    // Exception syndrome registers.
    mrs     x16, esr_el1
    str     x16, [sp, #(16*8)]
    mrs     x16, elr_el1
    str     x16, [sp, #(17*8)]
    mrs     x16, spsr_el1
    str     x16, [sp, #(19*8)]
    mrs     x16, far_el1
    str     x16, [sp, #(38*8)]
    str     xzr, [sp, #(21*8)]
    str     xzr, [sp, #(39*8)]

    // frame[18] is a synthetic CS: 3 when the exception came from EL0.
    // frame[20] stores the stack pointer that was in use before the trap.
    mrs     x17, spsr_el1
    and     x17, x17, #0xf
    cbnz    x17, 1f
    mov     x17, #3
    str     x17, [sp, #(18*8)]
    mrs     x17, sp_el0
    str     x17, [sp, #(20*8)]
    b       2f
1:
    mov     x17, #0
    str     x17, [sp, #(18*8)]
    add     x17, sp, #FRAME_SIZE
    str     x17, [sp, #(20*8)]
2:
    ldr     x0, [sp, #(15*8)]           // vector
    ldr     x1, [sp, #(16*8)]           // esr
    mov     x2, sp                      // frame
    bl      knx_interrupt_dispatch
    b       knx_restore_and_eret
.size knx_exception_enter, .-knx_exception_enter

.global knx_restore_and_eret
.type knx_restore_and_eret,@function
knx_restore_and_eret:
    mov     sp, x0                      // x0 = frame selected by the scheduler
    ldr     x16, [sp, #(20*8)]
    msr     sp_el0, x16
    ldr     x16, [sp, #(17*8)]          // elr_el1
    ldr     x17, [sp, #(19*8)]          // spsr_el1
    msr     elr_el1, x16
    msr     spsr_el1, x17

    ldp     x19, x20, [sp, #(0*8)]
    ldp     x21, x22, [sp, #(2*8)]
    ldp     x23, x24, [sp, #(4*8)]
    ldp     x5,  x4,  [sp, #(6*8)]
    ldp     x1,  x0,  [sp, #(8*8)]
    ldp     x25, x2,  [sp, #(10*8)]
    ldp     x3,  x26, [sp, #(12*8)]
    ldr     x8,       [sp, #(14*8)]
    ldp     x17, x18, [sp, #(36*8)]
    ldp     x27, x28, [sp, #(22*8)]
    ldp     x29, x30, [sp, #(24*8)]
    ldp     x6,  x7,  [sp, #(26*8)]
    ldp     x9,  x10, [sp, #(28*8)]
    ldp     x11, x12, [sp, #(30*8)]
    ldp     x13, x14, [sp, #(32*8)]
    ldp     x15, x16, [sp, #(34*8)]
    add     sp, sp, #FRAME_SIZE
    eret
.size knx_restore_and_eret, .-knx_restore_and_eret

.section .note.GNU-stack,"",@progbits
