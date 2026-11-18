#include "knx.h"
#include "knx_syscall.h"

/*
 * AArch64 shell.  Input arrives through the event rings owned by interrupts.c,
 * time comes from the architectural counter, and reboots go through PSCI.
 */

static void wait_for_interrupt(void) { knx_halt(); }

static char read_char(void) {
    for (;;) {
        if (knx_console_owned()) {
            wait_for_interrupt();
            continue;
        }
        uint64_t flags = knx_irq_flags_save();
        char c = knx_serial_read_char();
        if (!c)
            c = knx_keyboard_read();
        knx_irq_flags_restore(flags);
        if (c)
            return c;
        wait_for_interrupt();
    }
}

static int equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static void line(const char *s) {
    knx_puts(s);
    knx_puts("\n");
}

static void show_memory(knx_boot_info *info) {
    knx_puts("Total usable memory: ");
    knx_print_u64(knx_total_memory() / 1048576);
    knx_puts(" MiB\nAvailable memory: ");
    knx_print_u64(knx_free_memory() / 1048576);
    knx_puts(" MiB\nMap regions: ");
    knx_print_u64(info ? info->memory_region_count : 0);
    knx_puts("\n");
}

static int parse_uint(const char **p, uint64_t *out) {
    while (**p == ' ')
        ++*p;
    if (**p < '0' || **p > '9')
        return 0;
    uint64_t n = 0;
    while (**p >= '0' && **p <= '9') {
        unsigned d = (unsigned)(*(*p)++ - '0');
        if (n > (UINT64_MAX - d) / 10)
            return 0;
        n = n * 10 + d;
    }
    *out = n;
    return 1;
}

static void calc(const char *s) {
    uint64_t a, b, r;
    if (!parse_uint(&s, &a)) {
        line("calc: expected: calc NUMBER OP NUMBER");
        return;
    }
    while (*s == ' ')
        ++s;
    char op = *s++;
    while (*s == ' ')
        ++s;
    if (!parse_uint(&s, &b)) {
        line("calc: missing or invalid second number");
        return;
    }
    while (*s == ' ')
        ++s;
    if (*s) {
        line("calc: unexpected trailing input");
        return;
    }
    if (op == '+') {
        if (UINT64_MAX - a < b) {
            line("calc: overflow");
            return;
        }
        r = a + b;
    } else if (op == '-') {
        if (b > a) {
            knx_putc('-');
            knx_print_u64(b - a);
            knx_puts("\n");
            return;
        }
        r = a - b;
    } else if (op == '*') {
        if (b && a > UINT64_MAX / b) {
            line("calc: overflow");
            return;
        }
        r = a * b;
    } else if (op == '/') {
        if (!b) {
            line("calc: division by zero");
            return;
        }
        r = a / b;
    } else {
        line("calc: operator must be +, -, *, or /");
        return;
    }
    knx_print_u64(r);
    knx_puts("\n");
}

/* Report uptime from the architectural counter. */
static void show_date(void) {
    uint64_t hz = knx_tsc_hz();
    uint64_t secs = hz ? knx_read_tsc() / hz : 0;
    knx_puts("System uptime: ");
    knx_print_u64(secs / 3600);
    knx_puts("h ");
    knx_print_u64((secs / 60) % 60);
    knx_puts("m ");
    knx_print_u64(secs % 60);
    knx_puts("s (architectural timer)\n");
}

static void show_uptime(void) {
    uint64_t secs = knx_ticks() / 100;
    knx_print_u64(secs / 3600);
    knx_puts("h ");
    knx_print_u64((secs / 60) % 60);
    knx_puts("m ");
    knx_print_u64(secs % 60);
    knx_puts("s\n");
}

static void build_kfs_path(const char *name, char *path, unsigned size) {
    (void)size;
    char rel[128];
    unsigned len = 0;
    const char *p = name;
    while (*p == '/') ++p;
    while (*p && *p != ' ' && len < sizeof(rel) - 1) rel[len++] = *p++;
    rel[len] = 0;

    if (len > 4 && rel[0] == 'u' && rel[1] == 'f' && rel[2] == 's' && rel[3] == '/') {
        unsigned plen = 0;
        path[plen++] = '/'; path[plen++] = 'u'; path[plen++] = 'f'; path[plen++] = 's'; path[plen++] = '/';
        for (unsigned i = 4; i < len; ++i) path[plen++] = rel[i];
        path[plen] = 0;
        return;
    }
    if (len >= 3 && rel[0] == 'u' && rel[1] == 'f' && rel[2] == 's' && len == 3) {
        path[0] = '/'; path[1] = 'u'; path[2] = 'f'; path[3] = 's'; path[4] = 0;
        return;
    }

    if (len > 4 && rel[0] == 'k' && rel[1] == 'f' && rel[2] == 's' && rel[3] == '/') {
        unsigned plen = 0;
        path[plen++] = '/'; path[plen++] = 'k'; path[plen++] = 'f'; path[plen++] = 's'; path[plen++] = '/';
        for (unsigned i = 4; i < len; ++i) path[plen++] = rel[i];
        path[plen] = 0;
        return;
    }
    int absolute = (name[0] == '/');
    if (len >= 3 && rel[0] == 'k' && rel[1] == 'f' && rel[2] == 's' && len == 3) {
        path[0] = '/'; path[1] = 'k'; path[2] = 'f'; path[3] = 's'; path[4] = 0;
        return;
    }

    (void)absolute;
    unsigned plen = 0;
    path[plen++] = '/';
    for (unsigned i = 0; i < len; ++i) path[plen++] = rel[i];
    path[plen] = 0;
}

static void cmd_ls(const char *arg) {
    char mount_buf[128];
    const char *mount = "/";
    if (arg) {
        while (*arg == ' ') ++arg;
        if (!*arg) {
            mount = "/";
        } else if (arg[0] == 'k' && arg[1] == 'f' && arg[2] == 's') {
            mount = "/kfs";
        } else if (arg[0] == 'u' && arg[1] == 'f' && arg[2] == 's') {
            mount = "/ufs";
        } else if (arg[0] == '/' && arg[1] == 'k' && arg[2] == 'f' && arg[3] == 's') {
            mount = "/kfs";
        } else if (arg[0] == '/' && arg[1] == 'u' && arg[2] == 'f' && arg[3] == 's') {
            mount = "/ufs";
        } else if (arg[0] == '/' && arg[1] == '/') {
            mount = "/";
        } else if (arg[0] == '/') {
            unsigned n = 0;
            while (arg[n] && arg[n] != ' ' && n + 1 < sizeof(mount_buf)) {
                mount_buf[n] = arg[n];
                ++n;
            }
            mount_buf[n] = 0;
            mount = mount_buf;
        } else {
            unsigned n = 0;
            mount_buf[n++] = '/';
            while (arg[n - 1] && arg[n - 1] != ' ' && n + 1 < sizeof(mount_buf)) {
                mount_buf[n] = arg[n - 1];
                ++n;
            }
            mount_buf[n] = 0;
            mount = mount_buf;
        }
    }
    struct knx_vnode *root = 0;
    if (knx_vfs_resolve(mount, &root) != 0 || !root || !root->ops || !root->ops->readdir) {
        line("No filesystem is currently installed.");
        return;
    }
    knx_puts("Filesystem: ");
    knx_puts(mount);
    knx_puts("\n");
    for (uint64_t i = 0; ; ++i) {
        struct knx_vfs_dirent entry;
        if (knx_vfs_readdir(root, i, &entry) < 0)
            break;
        char path[128];
        unsigned len = 0;
        const char *m = mount;
        while (*m && len + 1 < sizeof(path)) path[len++] = *m++;
        path[len++] = '/';
        unsigned j = 0;
        while (entry.name[j] && entry.name[j] != ' ' && len + 1 < sizeof(path)) path[len++] = entry.name[j++];
        path[len] = 0;
        struct knx_vnode *node = 0;
        struct knx_vfs_stat st = { 0 };
        int have_size = 0;
        if (knx_vfs_resolve(path, &node) == 0 && node && node->ops && node->ops->stat) {
            have_size = node->ops->stat(node, &st) == 0;
        }
        knx_puts(entry.name);
        if (have_size) {
            knx_puts("  ");
            knx_print_u64(st.size);
        }
        knx_puts("\n");
    }
}

static void cmd_cat(const char *name) {
    char path[160];
    build_kfs_path(name, path, sizeof(path));
    struct knx_vnode *node = 0;
    if (knx_vfs_resolve(path, &node) != 0 || !node) {
        line("File not found.");
        return;
    }
    static char buffer[512];
    struct knx_vfs_stat st = { 0 };
    uint64_t capacity = sizeof(buffer) - 1;
    if (node->ops && node->ops->stat) {
        node->ops->stat(node, &st);
        if (st.size < capacity) capacity = st.size;
    }
    int64_t got = knx_vfs_read(node, 0, buffer, capacity);
    if (got <= 0) {
        line("Read failed.");
        return;
    }
    buffer[got] = 0;
    knx_puts(buffer);
    knx_puts("\n");
}

static void cmd_exec(const char *command) {
    char path[KNX_VFS_PATH_MAX];
    char argv[8][48];
    const char *vector[9];
    unsigned argc = 0;
    while (*command == ' ') ++command;
    unsigned plen = 0;
    while (command[plen] && command[plen] != ' ' && plen + 1 < sizeof(path)) {
        path[plen] = command[plen];
        ++plen;
    }
    path[plen] = 0;
    if (!plen) { line("Usage: exec PATH [ARG ...]"); return; }
    if (path[0] != '/') {
        char absolute[KNX_VFS_PATH_MAX];
        build_kfs_path(path, absolute, sizeof(absolute));
        unsigned i = 0; while (absolute[i] && i + 1 < sizeof(path)) { path[i] = absolute[i]; ++i; }
        path[i] = 0;
    }
    command += plen;
    while (*command && argc < 8) {
        while (*command == ' ') ++command;
        if (!*command) break;
        unsigned n = 0;
        while (*command && *command != ' ' && n + 1 < sizeof(argv[0])) argv[argc][n++] = *command++;
        argv[argc][n] = 0;
        vector[argc] = argv[argc];
        ++argc;
    }
    if (!argc) { vector[0] = path; argc = 1; }
    vector[argc] = 0;
    const char *envp[] = {0};

    int pid = knx_kernel_exec(path, (const char *const *)vector, envp);
    if (pid < 0) {
        line("Exec failed.");
    } else {
        knx_puts("Started pid ");
        knx_print_u64((uint64_t)pid);
        line(".");
    }
}

static void execute(char *linebuf, knx_boot_info *info) {
    char *args = linebuf;
    while (*args && *args != ' ')
        ++args;
    if (*args) {
        *args++ = 0;
        while (*args == ' ')
            ++args;
    }
    if (equal(linebuf, "login")) {
        if (knx_login_attempt(args))
            line("Logged in locally as root.");
        else
            line("Login failed. The development console accepts `login root`.");
        return;
    }
    if (equal(linebuf, "whoami")) {
        if (knx_login_is_authenticated())
            line(knx_login_user());
        else
            line("Not logged in.");
        return;
    }
    if (!knx_login_is_authenticated()) {
        line("Login required. Use `login root` on this development console.");
        return;
    }
    if (equal(linebuf, "help"))
        line("login whoami help about clear version uname cpuinfo cpus meminfo uptime date tasks services ls cat exec "
             "vfstest echo calc rust cpp sysinfo acpi devices usb net ifconfig ping netstat syscalls beep reboot poweroff halt");
    else if (equal(linebuf, "about")) {
        knx_puts("KNX OS 1.0-alpha\n");
        knx_puts("Kernel: " KNX_KERNEL_VERSION "\n");
        knx_puts("AArch64 kernel built with C, C++, and Rust.\n");
        knx_puts("Drivers use MMIO; exceptions are handled by the AArch64 vector table.\n");
    }
    else if (equal(linebuf, "clear"))
        knx_clear();
    else if (equal(linebuf, "version"))
        line(KNX_KERNEL_VERSION);
    else if (equal(linebuf, "uname"))
        line(KNX_KERNEL_VERSION);
    else if (equal(linebuf, "cpuinfo"))
        knx_cpu_info();
    else if (equal(linebuf, "cpus")) {
        knx_cpu_topology();
        knx_smp_info();
    } else if (equal(linebuf, "meminfo"))
        show_memory(info);
    else if (equal(linebuf, "uptime"))
        show_uptime();
    else if (equal(linebuf, "tasks")) {
        knx_puts("Runnable threads: ");
        knx_print_u64(knx_task_count());
        knx_puts(", processes: ");
        knx_print_u64(knx_process_count());
        knx_puts(" (processes have isolated user page tables; threads share their process space), "
                 "online CPUs: ");
        knx_print_u64(knx_smp_cpu_count());
        knx_puts(", syscall calls: ");
        knx_print_u64(knx_task_syscalls());
        knx_puts("\n");
    } else if (equal(linebuf, "syscalls")) {
        knx_puts("Syscall audit totals (id: count):\n");
        for (unsigned id = 0; id <= 12; ++id) {
            knx_print_u64(id);
            knx_puts(": ");
            knx_print_u64(knx_task_syscall_audit(id));
            knx_puts(id == 12 ? "\n" : " | ");
        }
    } else if (equal(linebuf, "services"))
        knx_services_info();
    else if (equal(linebuf, "vfstest"))
        knx_vfs_selftest();
    else if (equal(linebuf, "date"))
        show_date();
    else if (equal(linebuf, "ls")) {
        cmd_ls(args);
    }
    else if (equal(linebuf, "cat")) {
        cmd_cat(args);
    }
    else if (equal(linebuf, "exec")) {
        cmd_exec(args);
    }
    else if (equal(linebuf, "echo")) {
        knx_puts(args);
        knx_puts("\n");
    } else if (equal(linebuf, "calc"))
        calc(args);
    else if (equal(linebuf, "beep"))
        line("beep: PC speaker is not available on AArch64.");
    else if (equal(linebuf, "rust")) {
        uint64_t r = knx_rust_add(40, 2);
        knx_puts("Rust module: 40 + 2 = ");
        knx_print_u64(r);
        knx_puts("\n");
    } else if (equal(linebuf, "cpp")) {
        if (!knx_cpp_command(linebuf))
            line("C++ demo returned no result.");
    } else if (equal(linebuf, "sysinfo")) {
        knx_puts(KNX_KERNEL_VERSION " | UEFI | ");
        knx_cpu_info();
        show_memory(info);
    } else if (equal(linebuf, "devices"))
        knx_devices_info();
    else if (equal(linebuf, "acpi"))
        knx_acpi_info();
    else if (equal(linebuf, "usb"))
        knx_usb_info();
    else if (equal(linebuf, "net"))
        knx_net_info();
    else if (equal(linebuf, "ifconfig"))
        knx_net_ifconfig();
    else if (equal(linebuf, "netstat"))
        knx_net_netstat();
    else if (equal(linebuf, "ping")) {
        uint32_t ip = 0;
        if (!*args || knx_net_parse_ip(args, &ip) != 0)
            line("Usage: ping A.B.C.D");
        else
            knx_net_ping(ip);
    }
    else if (equal(linebuf, "reboot")) {
        line("Requesting PSCI system reboot...");
        knx_warm_reboot();
        line("PSCI reset unavailable; halting.");
        for (;;)
            knx_halt();
    } else if (equal(linebuf, "poweroff")) {
        line("Requesting PSCI system power off...");
        if (knx_acpi_poweroff())
            line("PSCI power off failed. It is now safe to power down the system.");
        for (;;)
            knx_halt();
    } else if (equal(linebuf, "halt")) {
        line("System halted. It is now safe to power down the system.");
        for (;;)
            knx_halt();
    } else if (*linebuf) {
        knx_puts("Unknown command: ");
        knx_puts(linebuf);
        knx_puts(". Type help.\n");
    }
}

#define USER_CONSOLE_GRACE_TICKS 500

void knx_shell_run(knx_boot_info *info) {
    char *buffer = (char *)knx_heap_alloc(128);
    if (!buffer) {
        line("KNX shell: unable to allocate command buffer.");
        for (;;)
            knx_halt();
    }
    uint64_t console_deadline = knx_ticks() + USER_CONSOLE_GRACE_TICKS;
    int user_console_seen = 0;
    for (;;) {
        if (knx_console_owned()) {
            user_console_seen = 1;
            wait_for_interrupt();
            continue;
        }
        if (!user_console_seen &&
            (int64_t)(knx_ticks() - console_deadline) < 0) {
            wait_for_interrupt();
            continue;
        }
        knx_puts("knx> ");
        unsigned n = 0;
        for (;;) {
            char c = read_char();
            if (c == '\n') {
                knx_puts("\n");
                buffer[n] = 0;
                break;
            }
            if (c == '\b') {
                if (n) {
                    --n;
                    knx_puts("\b \b");
                }
            } else if (c >= 32 && c < 127 && n < 126) {
                buffer[n++] = c;
                knx_putc(c);
            }
        }
        execute(buffer, info);
    }
}
