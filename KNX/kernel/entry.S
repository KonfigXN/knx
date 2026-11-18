/*
 * AArch64 kernel entry.  UEFI hands us:
 *   x0 = knx_boot_info *
 *
 * The loader exits boot services with the MMU still on (EDK2 maps memory
 * 1:1), so C code can run immediately.  knx_vm_install() later replaces the
 * firmware tables with the kernel's own identity map.
 */
.section .text.entry
.global knx_entry
.type knx_entry,@function
knx_entry:
    mov     x19, x0                     // preserve boot info

    ldr     x20, =knx_entry_stack_top
    mov     sp, x20

    // Clear BSS (8-byte aligned by the linker script).
    ldr     x0, =_bss_start
    ldr     x1, =_bss_end
1:
    cmp     x0, x1
    b.ge    2f
    str     xzr, [x0], #8
    b       1b
2:
    ldr     x0, =knx_vector_table
    msr     vbar_el1, x0
    isb

    // If firmware left us at EL2, drop to EL1 before touching EL1 state.
    mrs     x0, CurrentEL
    and     x0, x0, #0xc
    cmp     x0, #8
    b.ne    3f
    mov     x20, sp
    bl      knx_drop_to_el1             // does not return
3:
    mov     x0, x19
    bl      kernel_main
4:
    wfi
    b       4b
.size knx_entry, .-knx_entry

/* EL2 -> EL1 transition.  Only used when firmware starts the loader at EL2. */
.section .text
.global knx_drop_to_el1
.type knx_drop_to_el1,@function
knx_drop_to_el1:
    mov     x0, xzr
    msr     hcr_el2, x0
    msr     cptr_el2, x0
    msr     hstr_el2, x0
    mov     x0, #(1 << 31)              // SCTLR_EL1.RW
    msr     sctlr_el1, x0
    msr     sp_el1, x20
    mov     x0, #0x3c5                  // EL1h, DAIF masked, AArch64
    msr     spsr_el2, x0
    adr     x0, 1f
    msr     elr_el2, x0
    isb
    eret
1:
    ret
.size knx_drop_to_el1, .-knx_drop_to_el1

/* Secondary CPU entry, called by PSCI CPU_ON with the mailbox pointer in x2. */
.section .text
.global knx_secondary_entry
.type knx_secondary_entry,@function
knx_secondary_entry:
    mov     x19, x2
    ldr     x20, [x19, #0]              // kernel stack top
    ldr     x21, [x19, #8]              // CPU index
    mov     sp, x20
    ldr     x0, =knx_vector_table
    msr     vbar_el1, x0
    isb
    mov     x0, x21
    bl      knx_arch_secondary_init     // enables the MMU, then calls knx_ap_main
1:
    wfi
    b       1b
.size knx_secondary_entry, .-knx_secondary_entry

.section .bss
.balign 16
.global knx_entry_stack
knx_entry_stack:
    .space 16384
.global knx_entry_stack_top
knx_entry_stack_top:

.section .note.GNU-stack,"",@progbits
