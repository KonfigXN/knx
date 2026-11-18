ENTRY(knx_entry)
PHDRS {
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}
SECTIONS {
  /* AArch64 UEFI loads the ELF at its physical address.  Kernel virtual and
   * physical addresses are identical; the kernel builds its own identity map
   * in TTBR0 during knx_vm_init(). */
  . = 0x40080000;
  .text : {
    *(.text.entry)
    *(.text.vectors)
    *(.text*)
  } :text
  . = ALIGN(4096);
  __user_text_start = .;
  .usertext : { *(.usertext*) } :text
  .usertext.data : { *(.usertext.data*) } :text
  . = ALIGN(4096);
  __user_text_end = .;
  .rodata : { *(.rodata*) } :text
  .data : { *(.data*) } :data
  . = ALIGN(4096);
  _bss_start = .;
  .bss : { *(COMMON) *(.bss*) } :data
  . = ALIGN(4096);
  __user_stack_start = .;
  .userstack (NOLOAD) : { *(.userstack*) } :data
  . = ALIGN(4096);
  __user_stack_end = .;
  . = ALIGN(4096);
  _bss_end = .;
  __kernel_end = .;

  ASSERT(__kernel_end <= 0x40800000, "kernel image exceeds the 8 MiB AArch64 window")
  /DISCARD/ : { *(.comment*) *(.eh_frame*) *(.note*) }
}
