# KNX Kernel

**KNX** is an original x86_64 operating system kernel developed from scratch using freestanding C, C++, Rust, and assembly. It is an independent kernel implementation containing no Linux kernel code.

This repository contains **the kernel and its boot infrastructure only**. The userspace environment, BusyBox, desktop components, and X11 stack are maintained separately in the main KNX operating-system project.

The build system compiles the kernel, BIOS and UEFI bootloaders, and a standalone, kernel-only bootable ISO. No userspace filesystem, root image, or additional data disk is generated.

**License:** GPL-3.0-only. See [LICENSE](LICENSE).

---

## Features and implementation status

| Subsystem | Implementation |
|---|---|
| Boot | BIOS stage 1/2, UEFI loader, E820/UEFI memory maps, kernel handoff |
| CPU | GDT, TSS, IDT, PIC/PIT, SMP discovery and AP startup, ACPI |
| Memory | Physical page allocator, kernel heap, paging, demand paging, copy-on-write |
| Tasks | Round-robin scheduler, process creation and termination, threads, ring 3, `int 0x80` syscalls |
| Storage | VFS, read-only UFS1 (primary), read-only KFS (fallback), ATA PIO |
| Devices | PCI, xHCI/OHCI/UHCI USB HID, PS/2, Intel I2C touchpad, GOP framebuffer |
| Networking | RTL8139 driver, minimal ARP/IP/ICMP/UDP implementation |
| Console | Serial, VGA and GOP output, kernel shell, login gate |
| Extensions | Experimental ELF64 `ET_REL` kernel extension loader |

### Kernel architecture

KNX currently follows a **monolithic kernel architecture**. Core functionality, including the scheduler, memory management, VFS, filesystems, device drivers, networking, console, shell, and extension loader, is linked directly into `kernel.elf`.

The extension loader is experimental and does not currently represent a separate driver or module architecture.

---

## Repository structure

```text
KNX/
├── boot/
│   ├── bios/             BIOS stage 1/2 and El Torito loader
│   └── uefi/             UEFI application and long-mode handoff
├── kernel/               Kernel entry, CPU, memory, interrupts,
│                         SMP, VFS, filesystems, drivers, networking,
│                         console, shell, tasks, Rust and C++
├── include/              Kernel headers and public boot/syscall ABI
├── docs/                 Architecture, boot, storage, processes,
│                         and licensing documentation
├── Makefile              Kernel, bootloaders and ISO build targets
└── toolchain.mk          Shared GCC/LLVM toolchain configuration
```

---

## Building KNX

### Requirements

- **LLVM/Clang and LLD** (`ld.lld`, `lld-link`) for freestanding `x86_64-elf`, `i386-elf`, and UEFI targets.
- **GCC/G++ and GNU binutils** for the alternative POSIX toolchain.
- **Rust** with the `x86_64-unknown-none` bare-metal standard library.
- **GNU Make**.
- **ISO utilities:** `xorriso`, mtools, and dosfstools (`mkfs.fat`, `mmd`, `mcopy`).

QEMU and OVMF are not build dependencies. This repository intentionally does not include VM launch scripts or an automated boot-testing framework.

### Build commands

```sh
cd KNX

make                 # Default stable profile
make stable          # Optimized stable build
make debug           # Debug build with symbols
make builds          # Build both profiles

make kernel          # Kernel ELF only
make kernel-bin      # Kernel ELF and raw binary
make iso             # Kernel-only bootable ISO

make clean           # Remove the current build tree
```

Build artifacts are written to `build/`, with separate directories for the stable and debug profiles.

The default build produces a hybrid BIOS/UEFI bootable ISO containing the bootloaders, `KERNEL.BIN`, README, and license. It does not contain a userspace environment.

The resulting ISO can be booted using compatible virtual machines or physical hardware.

### Windows development

KNX can also be built on Windows using LLVM and MSYS2, without requiring a cross-GCC installation.

The `toolchain.mk` configuration automatically selects native LLVM tools for the ELF targets, while MSYS2 provides GNU Make, `xorriso`, and mtools.

Use the included PowerShell build script:

```powershell
.\build-windows.ps1 stable
.\build-windows.ps1 debug
.\build-windows.ps1 builds
.\build-windows.ps1 kernel
.\build-windows.ps1 clean
```

Custom tool locations can be provided through:

- `KNX_MSYS2_ROOT`
- `KNX_LLVM_BIN`
- `KNX_RUST_BIN`
- `KNX_RUST_SYSROOT`

The Rust sysroot must include:

```text
lib/rustlib/x86_64-unknown-none/lib/libcore-*.rlib
```

---

## Boot protocol and handoff

Both the BIOS and UEFI loaders construct a `knx_boot_info` structure before transferring control to the kernel at physical address `0x100000`.

The boot information structure provides the kernel with the memory map and boot method required for initialization.

The UEFI loader additionally supplies:

- GOP framebuffer information and display geometry.
- ACPI RSDP pointer.
- Optional live logging handle.

The `initrd_base` and `initrd_size` fields support an in-memory UFS1 root filesystem supplied by an external operating-system bootloader.

The repository's own bootloaders currently initialize these fields to zero. When no root filesystem is provided, the kernel probes attached ATA devices and falls back to its built-in shell if no usable root is found.

See [docs/boot.md](KNX/docs/boot.md) for the complete boot protocol.

---

## System call interface

User-mode applications communicate with the kernel through the `int 0x80` software interrupt interface.

System calls return negative `errno` values on failure.

The public interface is defined in [`KNX/include/knx_syscall.h`](KNX/include/knx_syscall.h) and currently covers:

- Process and thread management.
- Virtual memory operations.
- File and directory-related I/O.
- Input handling.
- Framebuffer access.
- Shared memory.
- Audio operations.
- Networking.
- System diagnostics.

The syscall interface is designed around KNX's own kernel architecture. It provides a POSIX-influenced programming environment but does not implement a complete POSIX ABI.

**The syscall interface is currently experimental and is not considered stable.**

---

## Development status

KNX is under active development. The kernel already implements a broad range of low-level operating-system functionality, while several subsystems remain experimental or incomplete.

### Implemented

- BIOS and UEFI boot.
- Physical memory management and paging.
- Interrupt handling and timer-based scheduling.
- SMP processor discovery and startup.
- ACPI and PCI support.
- USB HID and PS/2 input.
- Read-only UFS1 and KFS support.
- Virtual filesystem layer.
- Ring 3 execution and system calls.
- RTL8139 networking with basic IP protocols.
- GOP framebuffer console.
- Built-in kernel shell.
- Experimental ELF64 extension loading.

### Incomplete or not yet implemented

- Writable UFS support.
- `readdir` implementation.
- TCP, DHCP, and DNS.
- Stable public kernel ABI.
- Complete security audit.
- Independent driver architecture.

For subsystem-specific information, see [KNX/docs](KNX/docs/README.md).

For development history, see [CHANGELOG.md](CHANGELOG.md). Note that the changelog predates the separation of the kernel and userspace repositories and also documents earlier userspace and X11 development.

---

## Contributing

KNX is intended to become an open-source kernel project, and contributions are welcome.

When contributing:

- Keep kernel implementations original. Do not copy Linux kernel code.
- Follow the existing coding conventions and project structure.
- Preserve compatibility with the established boot protocol and syscall interfaces unless a deliberate change is being introduced.
- Build and test changes before submitting them.
- Clearly document changes to kernel interfaces, memory management, and subsystem behavior.

KNX is an independently developed kernel, and contributions should help maintain its focus on a lightweight, modular-in-development, and reliable operating-system foundation.
