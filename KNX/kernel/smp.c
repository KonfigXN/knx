#include "knx.h"

#define AP_START 0x8000U
#define AP_MAILBOX 0x9000U
#define MAX_SECONDARIES 15U
#define MAX_CPUS (MAX_SECONDARIES + 1U)
#define IA32_APIC_BASE 0x1bU
#define IA32_APIC_BASE_ENABLE (1ULL << 11)
#define IA32_APIC_BASE_X2APIC (1ULL << 10)
#define X2APIC_MSR_BASE 0x800U
#define PIT_RELOAD 11932U

extern char knx_ap_trampoline_start, knx_ap_trampoline_end;

struct __attribute__((packed)) acpi_header {
    char signature[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oem_id[6], oem_table_id[8];
    uint32_t oem_revision, creator_id, creator_revision;
};

struct ap_boot_entry {
    uint64_t stack_top;
    uint32_t apic_id;
    uint32_t reserved;
} __attribute__((aligned(16)));

static uint64_t ap_stacks[MAX_SECONDARIES][512] __attribute__((aligned(4096)));
static uint64_t ap_irq_stacks[MAX_SECONDARIES][512] __attribute__((aligned(4096)));
static struct ap_boot_entry ap_boot_table[MAX_CPUS];
static volatile uint32_t ap_boot_count;
static volatile uint8_t ap_online[MAX_CPUS];
static volatile uint64_t online_count = 1;
static volatile uint32_t *lapic;
static uint32_t processor_ids[MAX_CPUS];
static unsigned processor_count;
static uint32_t bsp_apic_id;
static int x2apic_mode;

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0,%1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint8_t checksum(const volatile uint8_t *p, uint32_t length) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; ++i)
        sum = (uint8_t)(sum + p[i]);
    return sum;
}

static uint32_t read32(const volatile uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t read64(const volatile uint8_t *p) {
    return (uint64_t)read32(p) | (uint64_t)read32(p + 4) << 32;
}

static int rsdp_valid(const volatile uint8_t *p) {
    static const char signature[] = "RSD PTR ";
    for (unsigned i = 0; i < 8; ++i)
        if (p[i] != (uint8_t)signature[i])
            return 0;
    return checksum(p, 20) == 0;
}

static volatile uint8_t *find_rsdp(knx_boot_info *info) {
    if (info && info->acpi_rsdp) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)info->acpi_rsdp;
        if (rsdp_valid(p))
            return p;
    }
    for (uint32_t address = 0xe0000; address < 0x100000; address += 16) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)address;
        if (rsdp_valid(p))
            return p;
    }
    return 0;
}

static volatile struct acpi_header *find_table(uint64_t root, unsigned entry_size,
                                                const char signature[4]) {
    volatile struct acpi_header *r = (volatile struct acpi_header *)(uintptr_t)root;
    if (!r || r->length < sizeof(*r) || r->length > 0x100000 ||
        checksum((const volatile uint8_t *)r, r->length) != 0)
        return 0;
    uint32_t count = (r->length - sizeof(*r)) / entry_size;
    const volatile uint8_t *entries = (const volatile uint8_t *)r + sizeof(*r);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t address = entry_size == 8 ? read64(entries + i * 8) : read32(entries + i * 4);
        volatile struct acpi_header *h = (volatile struct acpi_header *)(uintptr_t)address;
        if (!h || h->length < sizeof(*h) || h->length > 0x100000 ||
            h->signature[0] != signature[0] || h->signature[1] != signature[1] ||
            h->signature[2] != signature[2] || h->signature[3] != signature[3] ||
            checksum((const volatile uint8_t *)h, h->length) != 0)
            continue;
        return h;
    }
    return 0;
}

static volatile struct acpi_header *find_madt(knx_boot_info *info) {
    volatile uint8_t *rsdp = find_rsdp(info);
    if (!rsdp)
        return 0;
    uint64_t root = read32(rsdp + 16);
    unsigned entry_size = 4;
    if (rsdp[15] >= 2) {
        uint32_t length = read32(rsdp + 20);
        if (length >= 36 && length <= 4096 && checksum(rsdp, length) == 0) {
            uint64_t xsdt = read64(rsdp + 24);
            if (xsdt) {
                root = xsdt;
                entry_size = 8;
            }
        }
    }
    if (!root)
        return 0;
    return find_table(root, entry_size, "APIC");
}

static int parse_madt(uint64_t *lapic_address, knx_boot_info *info) {
    volatile struct acpi_header *madt = find_madt(info);
    if (!madt)
        return 0;
    uint32_t address = read32((const volatile uint8_t *)madt + 36);
    if (address & 0xfffff000U)
        *lapic_address = address & 0xfffff000U;
    uint32_t offset = 44;
    while (offset + 2 <= madt->length) {
        const volatile uint8_t *entry = (const volatile uint8_t *)madt + offset;
        uint8_t type = entry[0], length = entry[1];
        if (length < 2 || offset + length > madt->length)
            break;
        uint32_t id = 0, flags = 0;
        if (type == 0 && length >= 8) {
            id = entry[3];
            flags = read32(entry + 4);
        } else if (type == 9 && length >= 16) {
            id = read32(entry + 4);
            flags = read32(entry + 8);
        }
        if ((type == 0 || type == 9) && (flags & 3) && processor_count < MAX_CPUS) {
            unsigned seen = 0;
            while (seen < processor_count && processor_ids[seen] != id)
                ++seen;
            if (seen == processor_count)
                processor_ids[processor_count++] = id;
        }
        offset += length;
    }
    return processor_count != 0;
}

static uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static uint64_t read_cr3(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr3,%0" : "=r"(value));
    return value;
}

static void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                     : "memory");
}

static uint32_t apic_read(uint32_t offset) {
    if (read_msr(IA32_APIC_BASE) & IA32_APIC_BASE_X2APIC)
        return (uint32_t)read_msr(X2APIC_MSR_BASE + offset / 16);
    return lapic[offset / 4];
}

static void apic_write(uint32_t offset, uint32_t value) {
    if (read_msr(IA32_APIC_BASE) & IA32_APIC_BASE_X2APIC) {
        write_msr(X2APIC_MSR_BASE + offset / 16, value);
        return;
    }
    lapic[offset / 4] = value;
    (void)lapic[0x20 / 4];
}

static uint32_t apic_current_id(void) {
    uint32_t id = apic_read(0x20);
    return (read_msr(IA32_APIC_BASE) & IA32_APIC_BASE_X2APIC) ? id : id >> 24;
}

static int enable_x2apic(void) {
    uint64_t base = read_msr(IA32_APIC_BASE);
    if (base & IA32_APIC_BASE_X2APIC)
        return 1;
    write_msr(IA32_APIC_BASE, base | IA32_APIC_BASE_ENABLE | IA32_APIC_BASE_X2APIC);
    return (read_msr(IA32_APIC_BASE) & IA32_APIC_BASE_X2APIC) != 0;
}

static uint16_t pit_counter(void) {
    outb(0x43, 0x00);
    uint16_t value = inb(0x40);
    value |= (uint16_t)inb(0x40) << 8;
    return value;
}

static void delay_us(unsigned us) {
    unsigned counts = (unsigned)(((uint64_t)us * PIT_RELOAD) / 10000U);
    if (!counts)
        counts = 1;
    uint16_t previous = pit_counter();
    unsigned guard = counts + 10000U;
    while (counts && guard--) {
        uint16_t current = pit_counter();
        unsigned elapsed;
        if (current == previous)
            elapsed = 0;
        else if (current < previous)
            elapsed = previous - current;
        else
            elapsed = previous + (PIT_RELOAD - current);
        if (elapsed > counts)
            elapsed = counts;
        counts -= elapsed;
        previous = current;
    }
}

static int send_ipi(uint32_t id, uint32_t command) {
    for (unsigned tries = 0; tries < 1000000; ++tries) {
        if (!(apic_read(0x300) & (1u << 12)))
            break;
        __asm__ volatile("pause");
    }
    if (apic_read(0x300) & (1u << 12))
        return 0;
    if (read_msr(IA32_APIC_BASE) & IA32_APIC_BASE_X2APIC) {
        write_msr(X2APIC_MSR_BASE + 0x300 / 16, ((uint64_t)id << 32) | command);
        return 1;
    }
    if (id > 255)
        return 0;
    apic_write(0x310, id << 24);
    apic_write(0x300, command);
    return 1;
}

static unsigned ap_slot_for_id(uint32_t id) {
    for (uint32_t i = 0; i < ap_boot_count; ++i)
        if (ap_boot_table[i].apic_id == id)
            return i;
    return 0;
}

void knx_ap_main(uint64_t apic_id) __attribute__((noreturn));
void knx_ap_main(uint64_t apic_id) {
    unsigned cpu = ap_slot_for_id((uint32_t)apic_id);
    if (cpu == 0 || cpu >= MAX_CPUS)
        for (;;)
            __asm__ volatile("cli; hlt");

    knx_gdt_init_cpu(cpu);

    if (x2apic_mode)
        (void)enable_x2apic();

    knx_set_kernel_stack((uint64_t)(uintptr_t)&ap_irq_stacks[cpu - 1][512]);
    knx_idt_init();

    ap_online[cpu] = 1;
    __sync_fetch_and_add(&online_count, 1);
    knx_smp_ap_timer_init();
    for (;;)
        __asm__ volatile("sti; hlt");
}

void knx_smp_init(knx_boot_info *info) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(d & (1u << 9))) {
        knx_puts("SMP: local APIC unavailable.\n");
        return;
    }
    int x2apic_available = (c & (1u << 21)) != 0;
    uint64_t base = read_msr(IA32_APIC_BASE);
    uint64_t apic_address = base & 0xfffff000ULL;
    if (!parse_madt(&apic_address, info)) {
        knx_puts("SMP: ACPI MADT not found; secondary CPUs not started.\n");
        return;
    }

    uint64_t new_base = (base & ~0xfffff000ULL) | (apic_address & 0xfffff000ULL) |
                        IA32_APIC_BASE_ENABLE;
    if (x2apic_available)
        new_base |= IA32_APIC_BASE_X2APIC;
    write_msr(IA32_APIC_BASE, new_base);
    x2apic_mode = (read_msr(IA32_APIC_BASE) & IA32_APIC_BASE_X2APIC) != 0;
    lapic = (volatile uint32_t *)(uintptr_t)apic_address;

    bsp_apic_id = apic_current_id();
    ap_boot_table[0].apic_id = bsp_apic_id;
    ap_online[0] = 1;
    ap_boot_count = 1;

    unsigned seen = 0;
    while (seen < processor_count && processor_ids[seen] != bsp_apic_id)
        ++seen;
    if (seen == processor_count && processor_count < MAX_CPUS)
        processor_ids[processor_count++] = bsp_apic_id;

    apic_write(0xf0, 0x100 | 0xff);

    apic_write(0x3e0, 3);
    apic_write(0x320, (1u << 17) | 32);
    apic_write(0x380, 10000000);
    apic_write(0x350, 0x700);
    apic_write(0x370, 1u << 16);

    uint8_t *destination = (uint8_t *)(uintptr_t)AP_START;
    const uint8_t *source = (const uint8_t *)&knx_ap_trampoline_start;
    size_t trampoline_size = (size_t)((uintptr_t)&knx_ap_trampoline_end - (uintptr_t)source);
    if (trampoline_size > 4096) {
        knx_puts("SMP: AP trampoline is too large.\n");
        return;
    }
    for (size_t i = 0; i < 4096; ++i)
        destination[i] = i < trampoline_size ? source[i] : 0;

    for (unsigned i = 0; i < processor_count && ap_boot_count < MAX_CPUS; ++i) {
        uint32_t id = processor_ids[i];
        if (id == bsp_apic_id)
            continue;
        unsigned duplicate = 0;
        while (duplicate < ap_boot_count && ap_boot_table[duplicate].apic_id != id)
            ++duplicate;
        if (duplicate < ap_boot_count)
            continue;
        unsigned slot = ap_boot_count;
        ap_boot_table[slot].stack_top = (uint64_t)(uintptr_t)&ap_stacks[slot - 1][512];
        ap_boot_table[slot].apic_id = id;
        ap_boot_table[slot].reserved = slot;
        ++ap_boot_count;
    }

    *(volatile uint64_t *)(uintptr_t)(AP_MAILBOX + 0) = read_cr3();
    *(volatile uint64_t *)(uintptr_t)(AP_MAILBOX + 8) = (uint64_t)(uintptr_t)ap_boot_table;
    *(volatile uint64_t *)(uintptr_t)(AP_MAILBOX + 16) = (uint64_t)(uintptr_t)knx_ap_main;
    *(volatile uint64_t *)(uintptr_t)(AP_MAILBOX + 24) = (uint64_t)x2apic_mode;
    *(volatile uint64_t *)(uintptr_t)(AP_MAILBOX + 32) = ap_boot_count;

    unsigned started = 0;
    for (uint32_t slot = 1; slot < ap_boot_count; ++slot) {
        uint32_t id = ap_boot_table[slot].apic_id;

        if (!send_ipi(id, 0x0000c500))
            continue;
        delay_us(10000);
        (void)send_ipi(id, 0x00008500);
        delay_us(200);
        if (!send_ipi(id, 0x00000608))
            continue;
        delay_us(200);
        (void)send_ipi(id, 0x00000608);
        ++started;
    }

    for (unsigned i = 0; i < 100 && online_count < (uint64_t)started + 1; ++i)
        delay_us(10000);

    if (x2apic_available && !x2apic_mode)
        knx_log("[SMP]", "x2APIC enable failed; using xAPIC MMIO");
    if (x2apic_mode)
        knx_log("[SMP]", "x2APIC active on all online CPUs");
    knx_log("[SMP]", "Secondary CPU startup complete; see `cpus` for topology");
}

uint64_t knx_smp_cpu_index(void) {
    if (!lapic)
        return 0;
    return ap_slot_for_id(apic_current_id());
}

int knx_smp_is_bsp(void) {
    return knx_smp_cpu_index() == 0;
}

void knx_smp_ap_timer_init(void) {
    apic_write(0xf0, 0x100 | 0xff);
    apic_write(0x350, 1u << 16);
    apic_write(0x370, 1u << 16);
    apic_write(0x3e0, 3);

    apic_write(0x320, (1u << 16) | (1u << 17) | 32);
    apic_write(0x380, 10000000);
}

void knx_smp_ap_timer_eoi(void) {

    if (!lapic && !(read_msr(IA32_APIC_BASE) & IA32_APIC_BASE_X2APIC))
        return;
    apic_write(0xb0, 0);
}

uint64_t knx_smp_cpu_count(void) {
    return online_count;
}

void knx_smp_info(void) {
    knx_puts("APIC mode: ");
    knx_puts(x2apic_mode ? "x2APIC" : "xAPIC");
    knx_puts("; ACPI enabled APIC IDs: ");
    knx_print_u64(processor_count);
    knx_puts("; online CPUs: ");
    knx_print_u64(online_count);
    knx_puts("; BSP APIC ID: ");
    knx_print_u64(bsp_apic_id);
    knx_puts("\nAPIC IDs/status:");
    if (!ap_boot_count)
        knx_puts(" unavailable");
    for (uint32_t i = 0; i < ap_boot_count; ++i) {
        knx_puts(" ");
        knx_print_u64(ap_boot_table[i].apic_id);
        knx_puts(i == 0 ? "=BSP/online" : (ap_online[i] ? "=online" : "=offline"));
    }
    knx_puts("\n");
}
