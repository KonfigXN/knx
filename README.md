# KNX Kernel

**KNX** is an original AArch64 (arm64) operating system kernel developed from scratch using freestanding C, C++, and assembly, with Rust helpers currently built through a C fallback. It is an independent kernel implementation containing no Linux kernel code.

The platform support is **AArch64 only**: the tree builds exclusively for 64-bit ARM with UEFI firmware.

This repository contains **the kernel and its UEFI boot infrastructure only**. The userspace environment, BusyBox, desktop components, and X11 stack are maintained separately in the main KNX operating-system project.

The build system compiles the kernel, the AArch64 UEFI loader (`BOOTAA64.EFI`), and a standalone, kernel-only bootable ISO. No userspace filesystem, root image, or additional data disk is generated.

**License:** GPL-3.0-only. See [LICENSE](LICENSE).

---

## Features and implementation status

| Subsystem | Implementation |
|---|---|
| Boot | AArch64 UEFI loader, UEFI memory map, ELF64 kernel loading, `knx_boot_info` handoff |
| CPU | GICv2 interrupt controller, ARM generic timer, SMP via PSCI, ACPI |
| Memory | Physical page allocator, kernel heap, AArch64 translation tables, demand paging, copy-on-write |
| Tasks | Round-robin scheduler, process creation and termination, threads, EL0, `svc #0` syscalls |
| Storage | VFS, read-only UFS1 (primary), read-only KFS (fallback), in-memory root image |
| Devices | PCI Express (ECAM), xHCI USB host, GOP framebuffer, AArch64 generic timer |
| Networking | Intel e1000 (MMIO), minimal ARP/IP/ICMP/UDP implementation |
| Console | GOP framebuffer console, kernel shell, login gate |
| Extensions | Experimental ELF64 `ET_REL` kernel extension loader |

### Kernel architecture

KNX currently follows a **monolithic kernel architecture**. Core functionality, including the scheduler, memory management, VFS, filesystems, device drivers, networking, console, shell, and extension loader, is linked directly into `kernel.elf`.

The extension loader is experimental and does not currently represent a separate driver or module architecture.

---

## Repository structure

```text
KNX/
├── boot/
│   └── uefi/             AArch64 UEFI application (BOOTAA64.EFI)
├── kernel/               Kernel entry, CPU, memory, interrupts,
│                         SMP, VFS, filesystems, drivers, networking,
│                         console, shell, tasks, and C++ helpers
├── include/              Kernel headers and public boot/syscall ABI
├── docs/                 Architecture, boot, storage, processes,
│                         and licensing documentation
├── Makefile              Kernel, UEFI loader and ISO build targets
└── toolchain.mk          Shared AArch64 LLVM/GCC toolchain configuration
```

---

## Building KNX

### Requirements

- **LLVM/Clang and LLD** for freestanding `aarch64-elf` objects and the `aarch64-pc-windows-msvc` UEFI application (`lld-link -machine:arm64 -subsystem:efi_application`).
- **GCC/G++ and GNU binutils** for an alternative AArch64 POSIX toolchain.
- **Rust** tooling is optional: the two exported Rust ABI helpers are currently provided by `kernel/rust_stub.c` because an `aarch64-unknown-none` core sysroot is not assumed to be available. No Rust object is compiled by the Makefile at present.
- **GNU Make**.
- **ISO utilities:** `xorriso`, mtools, and dosfstools (`mkfs.fat`, `mmd`, `mcopy`).

QEMU and EDK2/OVMF are not build dependencies. This repository intentionally does not include VM launch scripts or an automated boot-testing framework.

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

The default build produces a UEFI-only bootable ISO containing `EFI/BOOT/BOOTAA64.EFI`, `EFI/KNX/KERNEL.BIN`, README, and license. It does not contain a userspace environment or legacy firmware boot entries.

The resulting ISO can be booted on AArch64 systems using compatible UEFI firmware (for example, QEMU `virt` with EDK2).

### Windows development

KNX can also be built on Windows using LLVM and MSYS2, without requiring a cross-GCC installation.

The `toolchain.mk` configuration selects native LLVM tools for the AArch64 targets, while MSYS2 provides GNU Make, `xorriso`, and mtools.

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

`KNX_RUST_SYSROOT` is forward-looking configuration for an `aarch64-unknown-none` core sysroot; the current kernel build does not compile Rust sources.

---

## Boot protocol and handoff

The UEFI loader looks for `EFI\KNX\KERNEL.BIN` (or `KERNEL.ELF`), parses its ELF64 program headers, loads each `PT_LOAD` segment at its physical address, and calls the kernel entry `knx_entry` directly with the MMU still enabled. The kernel ELF is linked at `0x40080000` with physical and virtual addresses equal; the kernel installs its own AArch64 translation tables and stack in `entry.S`, and drops from EL2 to EL1 itself when firmware starts the loader at EL2.

The loader constructs a `knx_boot_info` structure containing the UEFI-derived memory map, GOP framebuffer geometry, ACPI RSDP pointer, optional initial RAM image, and boot flags. The boot information structure lives in pages allocated as `EfiLoaderData` so it survives `ExitBootServices`.

The `initrd_base` and `initrd_size` fields support an in-memory root filesystem. The UEFI loader fills them when `EFI\KNX\USER.IMG` is present; otherwise they are zero. When no root filesystem is provided, the kernel probes its built-in filesystems and falls back to its built-in shell.

See [docs/boot.md](KNX/docs/boot.md) for the complete boot protocol.

---

## System call interface

User-mode applications communicate with the kernel through the AArch64 `svc #0` supervisor-call ABI.

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

- AArch64 UEFI boot with ELF64 kernel loading.
- Physical memory management and AArch64 translation tables.
- GICv2 interrupt handling and generic-timer scheduling.
- PSCI-based SMP secondary-core startup.
- ACPI support.
- PCI Express (ECAM) device discovery and xHCI USB host support.
- Read-only UFS1 and KFS filesystems.
- Virtual filesystem layer.
- EL0 execution and `svc #0` system calls.
- Intel e1000 networking with basic IP protocols.
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
- Native AArch64 Rust core sysroot (helpers currently use a C fallback).

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
