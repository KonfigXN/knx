.code64
.text
.globl knx_efi_handoff
knx_efi_handoff:
    movq %rcx, %rsp
    andq $-16, %rsp
    movq %r8, %rdi
    jmp *%rdx
