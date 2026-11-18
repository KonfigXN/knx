#include "knx.h"

static void boot_halt(const char *message) {
    knx_log("[ERROR]", message);
    for (;;)
        knx_halt();
}

void kernel_main(knx_boot_info *info) {
    knx_console_early_init();
    knx_puts("KNX OS " KNX_KERNEL_VERSION "\n");
#ifndef KNX_QUIET_LOGS
    knx_puts("Copyright (c) The KonfigX Team\n");
#endif
    knx_log("[BOOT]", "UEFI handoff to kernel");
    if (!info || info->magic != KNX_BOOT_MAGIC || info->version != KNX_BOOT_VERSION ||
        !info->memory_map || !info->memory_region_count)
        boot_halt("Invalid or incomplete bootloader information");
    knx_log("[BOOT]", "Bootloader information received");
    knx_puts("[BOOT] flags=");
    knx_print_u64(info->boot_flags);
    knx_puts("\n");

    knx_log("[CORE]", "Installing CPU tables and bootstrap memory...");
    /* Install the per-CPU kernel stack layer. */
    knx_percpu_init();

    knx_vm_init();

    knx_early_fb_stamp(info);

    knx_log("[CPU]", "Detecting processor features...");
    knx_cpu_init();
    knx_log("[CPU]", "CPU initialization complete");

    knx_log("[MEM]", "Detecting available memory and initializing physical allocator...");
    knx_memory_init(info);
    if (!knx_total_memory())
        boot_halt("Physical memory manager could not initialize from the boot memory map");
    knx_log("[MEM]", "Physical memory manager initialized");

    knx_log("[CONSOLE]", "Initializing kernel console...");
    knx_console_init(info);

    knx_fb_set_info(info);
    knx_log("[CONSOLE]", "Console initialized");

    if (knx_vm_shared_init())
        knx_log("[WARN]", "shared-memory window unavailable");
    if (knx_heap_selftest())
        knx_log("[TEST]", "heap allocator PASS");
    else
        knx_log("[TEST]", "heap allocator FAIL");

    knx_log("[INT]", "Installing Interrupt Descriptor Table...");
    knx_exceptions_init();

    knx_log("[VFS]", "Initializing virtual filesystem...");
    knx_vfs_init();

    knx_log("[UFS]", "Probing for UFS1 volume (-- primary root, read-only)");
    if (info->initrd_base && info->initrd_size >= 8192 &&
        info->initrd_base + info->initrd_size > info->initrd_base &&
        info->initrd_base + info->initrd_size <= (64ULL << 30))
        knx_ufs_memdisk((const void *)(uintptr_t)info->initrd_base, info->initrd_size);
    knx_ufs_init();
    if (knx_ufs_ready())
        knx_log("[ROOT]", "UFS mounted at / (read-only, primary)");
    else
        knx_log("[ROOT]", "UFS not mounted, KFS is root (fallback)");
    knx_log("[KFS]", "Probing for KFS volume (-- fallback at /kfs)");
    knx_kfs_init();

    if (knx_ufs_ready() || knx_kfs_ready()) {
        if (knx_root_mount_test())
            knx_log("[TEST]", "stage1 root mount PASS");
        else
            knx_log("[TEST]", "stage1 root mount FAIL");
        if (knx_kext_selftest())
            knx_log("[TEST]", "stage2a kext parse PASS");
        else
            knx_log("[TEST]", "stage2a kext parse FAIL");
        if (knx_kext_load_file("/sys/ext/hello.kext") == 0)
            knx_log("[TEST]", "stage2b1 kext load PASS");
        else
            knx_log("[TEST]", "stage2b1 kext load FAIL");
        if (knx_kext_unload("hello") == 0)
            knx_log("[TEST]", "stage2 extension load/unload PASS");
        else
            knx_log("[TEST]", "stage2 extension load/unload FAIL");
    } else {
        knx_log("[ROOT]", "No root filesystem supplied; skipping filesystem and extension checks");
    }

    knx_log("[DRIVER]", "Starting kernel driver and platform services...");
    if (!knx_services_init(info))
        boot_halt("A critical boot service failed to start");

    knx_log("[INIT]", "Starting scheduler and user-mode init (PID 1)");
    knx_start_user_tasks();
    if (knx_task_count() <= 1)
        knx_log("[WARN]", "No Ring 3 tasks were started");
    else
        knx_log("[TASK]", "Scheduler and Ring 3 tasks initialized");

    knx_log("[TASK]", "Running Ring 3 lifecycle and ELF checks...");
    knx_task_selftest_wait();
    if (knx_login_init())
        knx_log("[TEST]", "stage4 login PASS");
    else
        knx_log("[TEST]", "stage4 login FAIL");
    knx_log("[TUI]", "Starting text console and kernel shell");
    knx_puts("Ready (UEFI). Type help for commands.\n");
#ifndef KNX_QUIET_LOGS
    knx_log("[BOOT]", "Starting kernel main loop...");
#endif
    knx_shell_run(info);
}
