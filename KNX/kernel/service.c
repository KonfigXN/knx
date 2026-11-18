#include "knx.h"

#define SERVICE_MAX 16

struct service_entry {
    const char *name;
    knx_service_start_fn start;
    uint32_t dependencies;
    uint8_t state;
    uint8_t flags;
};

static struct service_entry services[SERVICE_MAX];
static unsigned service_count;
static unsigned service_failures;
static int services_started;

static int same_name(const char *a, const char *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static void service_log(const char *name, const char *message) {
    knx_puts("[SERVICE] ");
    knx_puts(name);
    knx_puts(": ");
    knx_puts(message);
    knx_puts("\n");
}

int knx_service_register_ex(const char *name, uint32_t dependencies, uint8_t flags,
                            knx_service_start_fn start) {
    if (!name || !*name || !start || service_count >= SERVICE_MAX ||
        (dependencies >> service_count) || services_started)
        return -1;
    for (unsigned i = 0; i < service_count; ++i)
        if (same_name(services[i].name, name))
            return -1;
    unsigned id = service_count++;
    services[id] = (struct service_entry){name, start, dependencies,
                                          KNX_SERVICE_REGISTERED, flags};
    return (int)id;
}

int knx_service_register(const char *name, uint32_t dependencies,
                         knx_service_start_fn start) {
    return knx_service_register_ex(name, dependencies, 0, start);
}

static int start_acpi(knx_boot_info *info) {
    knx_log("[ACPI]", "Detecting ACPI tables...");
    knx_acpi_init(info);
    knx_log("[ACPI]", "ACPI service initialized");
    knx_log("[ACPI]", "ACPI discovery complete");
    return 1;
}
static int start_smp(knx_boot_info *info) {
    knx_log("[CPU]", "Starting secondary processors...");
    knx_smp_init(info);
    if (knx_smp_cpu_count() > 1)
        knx_log("[CPU]", "Secondary processors are online");
    else
        knx_log("[WARN]", "No secondary processor is online; continuing on the BSP");
    return 1;
}
static int start_devices(knx_boot_info *info) {
    (void)info;
    knx_log("[PCI]", "Scanning PCI devices and probing registered drivers...");
    knx_devices_init();
#ifndef KNX_QUIET_LOGS

    knx_devices_info();
#endif
    knx_log("[PCI]", "PCI scan complete");
    return 1;
}
static int start_interrupts(knx_boot_info *info) {
    (void)info;
    knx_log("[INT]", "Configuring PIC and timer interrupts...");
    knx_interrupts_init();
    knx_log("[INT]", "Interrupt controller initialized");
    return 1;
}

static uint32_t service_bit(int id) {
    return id >= 0 ? 1u << (unsigned)id : 0;
}

static int register_builtin_services(void) {
    int acpi = knx_service_register("acpi-power", 0, start_acpi);
    int interrupts = knx_service_register_ex("interrupts", 0, KNX_SERVICE_CRITICAL,
                                             start_interrupts);
    int smp = knx_service_register("smp", service_bit(acpi) | service_bit(interrupts), start_smp);
    int devices = knx_service_register("pci-drivers", service_bit(interrupts), start_devices);
    return acpi >= 0 && interrupts >= 0 && smp >= 0 && devices >= 0;
}

static int critical_services_running(void) {
    for (unsigned i = 0; i < service_count; ++i)
        if ((services[i].flags & KNX_SERVICE_CRITICAL) &&
            services[i].state != KNX_SERVICE_RUNNING)
            return 0;
    return 1;
}

int knx_services_init(knx_boot_info *info) {
    if (services_started) {
        knx_log("[SERVICE]", "Boot service loader was already run");
        return critical_services_running();
    }
    service_count = 0;
    service_failures = 0;
    if (!register_builtin_services()) {
        services_started = 1;
        ++service_failures;
        knx_log("[SERVICE]", "Could not register required built-in boot services");
        return 0;
    }
    services_started = 1;

    uint32_t started = 0, failed = 0;
    for (unsigned pass = 0; pass < SERVICE_MAX; ++pass) {
        int progress = 0;
        for (unsigned i = 0; i < service_count; ++i) {
            struct service_entry *s = &services[i];
            if (s->state != KNX_SERVICE_REGISTERED)
                continue;
            if (s->dependencies & failed) {
                s->state = KNX_SERVICE_BLOCKED;
                failed |= 1u << i;
                ++service_failures;
                service_log(s->name, "blocked by failed dependency");
                progress = 1;
                continue;
            }
            if ((s->dependencies & started) != s->dependencies)
                continue;
            s->state = KNX_SERVICE_STARTING;
            service_log(s->name, "starting");
            if (s->start(info)) {
                s->state = KNX_SERVICE_RUNNING;
                started |= 1u << i;
                service_log(s->name, "running");
            } else {
                s->state = KNX_SERVICE_FAILED;
                failed |= 1u << i;
                ++service_failures;
                service_log(s->name, "start callback failed");
            }
            progress = 1;
        }
        if (!progress)
            break;
    }
    for (unsigned i = 0; i < service_count; ++i)
        if (services[i].state == KNX_SERVICE_REGISTERED) {
            services[i].state = KNX_SERVICE_BLOCKED;
            ++service_failures;
            service_log(services[i].name, "unresolved dependency");
        }

    return critical_services_running();
}

void knx_services_info(void) {
    knx_puts("Kernel services:\n");
    for (unsigned i = 0; i < service_count; ++i) {
        knx_puts("  ");
        knx_puts(services[i].name);
        knx_puts(": ");
        switch (services[i].state) {
        case KNX_SERVICE_REGISTERED: knx_puts("registered\n"); break;
        case KNX_SERVICE_STARTING: knx_puts("starting\n"); break;
        case KNX_SERVICE_RUNNING: knx_puts("running\n"); break;
        case KNX_SERVICE_FAILED: knx_puts("failed\n"); break;
        case KNX_SERVICE_BLOCKED: knx_puts("blocked\n"); break;
        }
    }
}

enum knx_service_state knx_service_state(unsigned id) {
    return id < service_count ? (enum knx_service_state)services[id].state : KNX_SERVICE_FAILED;
}

int knx_service_is_running(const char *name) {
    if (!name)
        return 0;
    for (unsigned i = 0; i < service_count; ++i)
        if (same_name(services[i].name, name))
            return services[i].state == KNX_SERVICE_RUNNING;
    return 0;
}

unsigned knx_service_count(void) {
    return service_count;
}

unsigned knx_service_failures(void) {
    return service_failures;
}
