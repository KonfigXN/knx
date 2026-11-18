# KNX Kernel

**KNX** is an original AArch64 (arm64) kernel developed from scratch using freestanding C, C++, and assembly, with Rust helpers currently built through a C fallback. It is an independent kernel implementation and contains no Linux kernel code.

The kernel is **AArch64 only**. The entire tree builds exclusively for 64-bit ARM with UEFI firmware.

This repository contains the **kernel source, the AArch64 UEFI bootloader, and a kernel-only bootable ISO**. Userspace, BusyBox, desktop environments, and the X11 stack are maintained as separate projects and are intentionally excluded from this build.

KNX is designed around a lightweight, monolithic architecture, with a focus on reliability, hardware support, and long-term development.

**License:** GPL-3.0-only. The complete license text is available in `LICENSE` at the repository root and is included as `COPYING` at the root of the generated ISO.

## Project Structure

```text
boot/
  uefi/              AArch64 UEFI application (EFI/BOOT/BOOTAA64.EFI)

kernel/              Core kernel implementation
                     CPU (GICv2, generic timer), memory management,
                     AArch64 translation tables, SMP via PSCI,
                     VFS, filesystems, drivers, networking,
                     console, shell, task management, C++ helpers

include/             Kernel headers and public interfaces
                     System call and boot information ABI

docs/                Architecture, boot process, storage,
                     UFS, process management and licensing

Makefile             Kernel, UEFI loader and ISO build system
toolchain.mk         Shared AArch64 LLVM/GCC toolchain configuration
LICENSE              GPL-3.0-only license (shipped as COPYING in the ISO)
```

## Building KNX

### Requirements

The build environment requires:

- Clang and LLD for `aarch64-elf` objects and the `aarch64-pc-windows-msvc` UEFI application
- GCC and GNU binutils for an alternative AArch64 POSIX toolchain
- GNU Make
- `xorriso`, mtools and dosfstools (for ISO generation)

Rust is not required for the current build. The two exported Rust ABI helpers are implemented in `kernel/rust_stub.c` because an `aarch64-unknown-none` core sysroot is not assumed to be available. The Makefile does not compile Rust sources at present.

### Build Targets

```sh
make                 # Build using the default stable profile
make stable          # Build the stable profile
make debug           # Build the debug profile
make builds          # Build both stable and debug profiles

make kernel          # Build kernel.elf only
make kernel-bin      # Build ELF and raw kernel.bin
make iso             # Generate the kernel-only bootable ISO

make clean           # Remove generated build artifacts
```

The default build produces:

```text
build/knx.iso
```

Profile-specific builds are placed in their respective directories:

```text
build/
  stable/
    knx.iso
  debug/
    knx.iso
```

### Windows

Windows builds are supported through the provided PowerShell build script.

From the repository root, run:

```powershell
../build-windows.ps1
```

The script configures the LLVM and MSYS2 toolchain before invoking the Makefile. Tool paths can be overridden with `KNX_MSYS2_ROOT`, `KNX_LLVM_BIN`, `KNX_RUST_BIN`, and `KNX_RUST_SYSROOT`. The latter is forward-looking configuration for an `aarch64-unknown-none` core sysroot; no Rust object is compiled by the current build.

## Boot Architecture

KNX boots through UEFI only. The AArch64 firmware loads `EFI/BOOT/BOOTAA64.EFI`, which locates `EFI/KNX/KERNEL.BIN` (or `KERNEL.ELF`), loads its ELF64 `PT_LOAD` segments at their physical addresses, and calls the kernel entry `knx_entry` directly while the MMU is still on.

The kernel ELF is linked at `0x40080000` with physical and virtual addresses equal. `entry.S` installs the kernel stack and AArch64 translation tables, and drops from EL2 to EL1 itself when firmware starts the loader at EL2. Interrupts are handled by GICv2, scheduling uses the ARM generic timer, and secondary CPUs are started through PSCI.

Both the loader and kernel use the `knx_boot_info` structure; this ABI includes the UEFI-derived memory map, GOP framebuffer information, ACPI RSDP pointer, optional initial RAM image, and boot flags.

### Root Filesystem Handoff

An external OS bootloader can provide an in-memory UFS1 root filesystem image through the following boot information fields:

- `initrd_base` — Physical base address of the root image.
- `initrd_size` — Size of the supplied image.

The UEFI loader fills these fields when `EFI\KNX\USER.IMG` is present; otherwise they are zero. The kernel can also boot without an initial root filesystem or attached storage device. In that configuration, it reaches its built-in shell while skipping checks that depend on filesystem availability.

The generated ISO contains:

- A UEFI boot entry with `EFI/BOOT/BOOTAA64.EFI`.
- The kernel image at `EFI/KNX/KERNEL.BIN`.
- The README and license.

It does not contain a userspace image or a separate data disk.

**Boot testing:** This repository does not include QEMU launch scripts or an automated boot-testing environment. The generated ISO can be launched on AArch64 UEFI firmware using external virtualization or hardware-testing tools.

## System Interfaces and ABI

KNX exposes project-specific interfaces for communication between the kernel and its callers.

The primary public interfaces are:

| Header | Purpose |
|---|---|
| `include/knx_syscall.h` | `svc #0` system call numbers and wrappers |
| `include/knx.h` | Boot information structure and related definitions |

The system call interface and boot ABI are currently under development and **are not considered stable**. Their definitions may change as the kernel evolves.

## Documentation

Technical documentation is maintained separately under `docs/`.

Start with:

```text
docs/README.md
```

It provides an index of the available documentation, covering kernel architecture, boot procedures, storage, UFS, process management, and other implementation details.

The changelog at the repository root predates the separation of the kernel and userspace projects. As a result, it also contains historical entries covering earlier userspace and X11 development.

---

**KNX Kernel — An independent AArch64 kernel implementation, built from the ground up.**
