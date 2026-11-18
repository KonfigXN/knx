#include "knx.h"
#include "knx_syscall.h"
struct __attribute__((packed)) idt_gate {
    uint16_t low;
    uint16_t selector;
    uint8_t ist;
    uint8_t flags;
    uint16_t mid;
    uint32_t high;
    uint32_t reserved;
};
struct __attribute__((packed)) idtr {
    uint16_t limit;
    uint64_t base;
};
static struct idt_gate idt[256] __attribute__((aligned(16)));
extern void knx_load_idt(const struct idtr *);
#define DECLARE_ISR(n) extern void isr##n(void)
DECLARE_ISR(0);
DECLARE_ISR(1);
DECLARE_ISR(2);
DECLARE_ISR(3);
DECLARE_ISR(4);
DECLARE_ISR(5);
DECLARE_ISR(6);
DECLARE_ISR(7);
DECLARE_ISR(8);
DECLARE_ISR(9);
DECLARE_ISR(10);
DECLARE_ISR(11);
DECLARE_ISR(12);
DECLARE_ISR(13);
DECLARE_ISR(14);
DECLARE_ISR(15);
DECLARE_ISR(16);
DECLARE_ISR(17);
DECLARE_ISR(18);
DECLARE_ISR(19);
DECLARE_ISR(20);
DECLARE_ISR(21);
DECLARE_ISR(22);
DECLARE_ISR(23);
DECLARE_ISR(24);
DECLARE_ISR(25);
DECLARE_ISR(26);
DECLARE_ISR(27);
DECLARE_ISR(28);
DECLARE_ISR(29);
DECLARE_ISR(30);
DECLARE_ISR(31);
DECLARE_ISR(32);
DECLARE_ISR(33);
DECLARE_ISR(34);
DECLARE_ISR(35);
DECLARE_ISR(36);
DECLARE_ISR(37);
DECLARE_ISR(38);
DECLARE_ISR(39);
DECLARE_ISR(40);
DECLARE_ISR(41);
DECLARE_ISR(42);
DECLARE_ISR(43);
DECLARE_ISR(44);
DECLARE_ISR(45);
DECLARE_ISR(46);
DECLARE_ISR(47);
DECLARE_ISR(128);
#undef DECLARE_ISR
static void set_gate(unsigned n, void (*fn)(void)) {
    uint64_t a = (uint64_t)(uintptr_t)fn;
    idt[n].low = (uint16_t)a;
    idt[n].selector = 8;
    idt[n].ist = 0;
    idt[n].flags = n == 128 ? 0xee : 0x8e;
    idt[n].mid = (uint16_t)(a >> 16);
    idt[n].high = (uint32_t)(a >> 32);
    idt[n].reserved = 0;
}
void knx_idt_init(void) {
    for (unsigned i = 0; i < 256; ++i) {
        idt[i].low = 0;
        idt[i].selector = 8;
        idt[i].ist = 0;
        idt[i].flags = 0x8e;
        idt[i].mid = 0;
        idt[i].high = 0;
        idt[i].reserved = 0;
    }
    void (*const e[32])(void) = {isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
                                 isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
                                 isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
                                 isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31};
    for (unsigned i = 0; i < 32; ++i)
        set_gate(i, e[i]);
    set_gate(32, isr32);
    set_gate(33, isr33);
    void (*const irq[14])(void) = {isr34, isr35, isr36, isr37, isr38, isr39, isr40,
                                   isr41, isr42, isr43, isr44, isr45, isr46, isr47};
    for (unsigned i = 0; i < 14; ++i)
        set_gate(34 + i, irq[i]);
    set_gate(128, isr128);
    struct idtr p = {sizeof(idt) - 1, (uint64_t)(uintptr_t)idt};
    knx_load_idt(&p);
}
static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}
static inline uint8_t inb(uint16_t p) {
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static int i8042_wait_input_empty(void) {
    for (unsigned i = 0; i < 100000; ++i) {
        uint8_t status = inb(0x64);
        if (status == 0xff)
            return 0;
        if (!(status & 2))
            return 1;
        __asm__ volatile("pause");
    }
    return 0;
}
static int i8042_wait_output_full(void) {
    for (unsigned i = 0; i < 100000; ++i) {
        uint8_t status = inb(0x64);
        if (status == 0xff)
            return 0;
        if (status & 1)
            return 1;
        __asm__ volatile("pause");
    }
    return 0;
}
static int i8042_write_command(uint8_t command) {
    if (!i8042_wait_input_empty())
        return 0;
    outb(0x64, command);
    return 1;
}

static int i8042_read_byte(uint8_t *out) {
    if (!i8042_wait_output_full())
        return 0;
    *out = inb(0x60);
    return 1;
}

static int i8042_mouse_command(uint8_t command) {
    if (!i8042_write_command(0xd4))
        return 0;
    if (!i8042_wait_input_empty())
        return 0;
    outb(0x60, command);
    uint8_t ack = 0;
    if (!i8042_read_byte(&ack))
        return 0;
    return ack == 0xfa;
}

void knx_warm_reboot(void) {
    __asm__ volatile("cli");
    if (!knx_acpi_reboot()) {
        for (unsigned i = 0; i < 1000000 && (inb(0x64) & 2); ++i)
            __asm__ volatile("pause");
        outb(0x64, 0xfe);
    }

    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile("lidt %0" : : "m"(null_idt));
    __asm__ volatile("int $3");
    for (;;)
        __asm__ volatile("hlt");
}

static int mouse_irq_active;
static volatile uint8_t mouse_dbg;
static volatile uint32_t mouse_irq_count;
static volatile uint32_t mouse_poll_bytes;

static void tdb_hex8(uint8_t v) {
    knx_putc("0123456789abcdef"[v >> 4]);
    knx_putc("0123456789abcdef"[v & 15]);
}

static int i8042_keyboard_init(void) {
    knx_log("DBG", "kbinit: start");
    if (inb(0x64) == 0xff) {
        knx_log("DBG", "kbinit: no i8042 (status ff)");
        return 0;
    }
    if (!i8042_wait_input_empty()) {
        knx_log("DBG", "kbinit: wait_input_empty failed pre-AD");
        return 0;
    }
    outb(0x64, 0xad);
    if (!i8042_wait_input_empty()) {
        knx_log("DBG", "kbinit: wait_input_empty failed post-AD");
        return 0;
    }

    for (unsigned i = 0; i < 1024 && (inb(0x64) & 1); ++i)
        (void)inb(0x60);
    outb(0x64, 0x20);
    if (!i8042_wait_output_full()) {
        knx_log("DBG", "kbinit: READ_CONFIG not acknowledged");
        return 0;
    }
    uint8_t config = inb(0x60);
    knx_log("DBG", "kbinit: config byte=0x");
    tdb_hex8(config);
    knx_putc('\n');

    config = (uint8_t)((config | 0x03 | 0x40) & ~(uint8_t)(0x10 | 0x20 | 0x80));
    if (!i8042_wait_input_empty())
        return 0;
    outb(0x64, 0x60);
    if (!i8042_wait_input_empty())
        return 0;
    outb(0x60, config);
    if (!i8042_wait_input_empty())
        return 0;
    outb(0x64, 0xae);
    if (!i8042_wait_input_empty())
        return 0;
    outb(0x60, 0xf4);
    int keyboard_ready = 0;
    if (i8042_wait_output_full())
        keyboard_ready = inb(0x60) == 0xfa;

    if (keyboard_ready && !i8042_wait_input_empty())
        keyboard_ready = 0;
    if (keyboard_ready) {
        outb(0x60, 0xf0);
        if (!i8042_wait_output_full() || inb(0x60) != 0xfa)
            keyboard_ready = 0;
    }
    if (keyboard_ready && !i8042_wait_input_empty())
        keyboard_ready = 0;
    if (keyboard_ready) {
        outb(0x60, 0x02);
        if (!i8042_wait_output_full() || inb(0x60) != 0xfa)
            keyboard_ready = 0;
    }

    if (keyboard_ready && !i8042_wait_input_empty())
        keyboard_ready = 0;
    if (keyboard_ready) {
        outb(0x60, 0xed);
        if (!i8042_wait_output_full() || inb(0x60) != 0xfa)
            keyboard_ready = 0;
    }
    if (keyboard_ready && !i8042_wait_input_empty())
        keyboard_ready = 0;
    if (keyboard_ready) {
        outb(0x60, 0x02);
        if (!i8042_wait_output_full() || inb(0x60) != 0xfa)
            keyboard_ready = 0;
    }

    mouse_irq_active = 0;
    if (i8042_write_command(0xa8)) {
        mouse_dbg = 1;
        knx_log("DBG", "mouse: 0xA8 accepted, status=0x");
        tdb_hex8(inb(0x64));
        knx_putc('\n');
        int mouse_ready = 0;
        for (int attempt = 0; attempt < 3 && !mouse_ready; ++attempt) {
            mouse_ready = i8042_mouse_command(0xf6);
            knx_log("DBG", "mouse: 0xF6 attempt, ready=");
            knx_putc(mouse_ready ? '1' : '0');
            knx_putc(' ');
            knx_log("DBG", "status=0x");
            tdb_hex8(inb(0x64));
            knx_putc('\n');
        }
        if (mouse_ready) {
            mouse_dbg = 2;
            mouse_ready = i8042_mouse_command(0xf4);
        }
        if (mouse_ready)
            mouse_dbg = 3;
        mouse_irq_active = mouse_ready;
        knx_log("DBG", "mouse: final active=0x");
        tdb_hex8((uint8_t)mouse_irq_active);
        knx_putc('\n');
    } else {
        knx_log("DBG", "mouse: 0xA8 write FAILED, status=0x");
        tdb_hex8(inb(0x64));
        knx_putc('\n');
    }
    return keyboard_ready;
}
static volatile uint64_t tick_count;
static volatile uint8_t key_queue[128];
static volatile uint32_t key_read, key_write;

struct key_event {
    uint8_t code;
    uint8_t pressed;
    uint16_t mods;
};
static volatile struct key_event key_events[128];
static volatile uint32_t key_ev_read, key_ev_write;
static volatile uint16_t key_mods;
static volatile int key_ev_ext;

#define MOUSE_RING 64
static volatile struct knx_mouse_event mouse_events[MOUSE_RING];
static volatile uint32_t mouse_ev_read, mouse_ev_write;
static volatile int32_t mouse_x = -1, mouse_y = -1;
static volatile uint8_t mouse_buttons;
static volatile uint8_t mouse_pkt[3];
static volatile int mouse_phase;

int knx_mouse_event_read(struct knx_mouse_event *event) {
    if (mouse_ev_read == mouse_ev_write)
        return 0;
    if (event)
        *event = mouse_events[mouse_ev_read & (MOUSE_RING - 1)];
    ++mouse_ev_read;
    return 1;
}

static void mouse_emit(uint8_t type) {
    if (mouse_ev_write - mouse_ev_read >= MOUSE_RING)
        return;

    struct knx_mouse_event ev;
    ev.x = mouse_x;
    ev.y = mouse_y;
    ev.buttons = mouse_buttons;
    ev.type = type;
    ev.reserved = 0;
    mouse_events[mouse_ev_write & (MOUSE_RING - 1)] = ev;
    ++mouse_ev_write;
}

void knx_mouse_event_emit(int32_t dx, int32_t dy, uint8_t buttons) {
    uint32_t width = 1280, height = 800;
    knx_fb_get_size(&width, &height);
    if (!width) width = 1280;
    if (!height) height = 800;
    if (mouse_x < 0 || mouse_y < 0) {
        mouse_x = (int32_t)width / 2;
        mouse_y = (int32_t)height / 2;
    }
    int32_t nx = mouse_x + dx;
    int32_t ny = mouse_y - dy;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx >= (int32_t)width) nx = (int32_t)width - 1;
    if (ny >= (int32_t)height) ny = (int32_t)height - 1;
    mouse_x = nx;
    mouse_y = ny;
    uint8_t mask = buttons & 0x07;
    uint8_t changed = (uint8_t)(mouse_buttons ^ mask);
    mouse_buttons = mask;
    if (dx || dy)
        mouse_emit(KNX_MOUSE_MOTION);
    if (changed & 0x07)
        mouse_emit((uint8_t)((mask & changed) ? KNX_MOUSE_PRESS : KNX_MOUSE_RELEASE));
}

static void mouse_packet_byte(uint8_t byte) {
    if (mouse_phase == 0) {

        if (!(byte & 0x08))
            return;
        mouse_pkt[0] = byte;
        mouse_phase = 1;
        return;
    }
    mouse_pkt[mouse_phase++] = byte;
    if (mouse_phase < 3)
        return;
    mouse_phase = 0;

    static int first_packet;
    if (!first_packet) {
        first_packet = 1;
        knx_puts("[MOUSE] first packet\n");
    }
    uint8_t flags = mouse_pkt[0];
    int32_t dx = (int8_t)mouse_pkt[1];
    int32_t dy = (int8_t)mouse_pkt[2];
    if (flags & 0x40)
        dx = 0;
    if (flags & 0x80)
        dy = 0;

    uint32_t width = 1280, height = 800;
    knx_fb_get_size(&width, &height);
    if (!width)
        width = 1280;
    if (!height)
        height = 800;
    if (mouse_x < 0 || mouse_y < 0) {
        mouse_x = (int32_t)width / 2;
        mouse_y = (int32_t)height / 2;
    }
    int32_t nx = mouse_x + dx;
    int32_t ny = mouse_y - dy;
    if (nx < 0)
        nx = 0;
    if (ny < 0)
        ny = 0;
    if (nx >= (int32_t)width)
        nx = (int32_t)width - 1;
    if (ny >= (int32_t)height)
        ny = (int32_t)height - 1;
    mouse_x = nx;
    mouse_y = ny;

    uint8_t buttons = flags & 0x07;
    uint8_t changed = (uint8_t)(mouse_buttons ^ buttons);
    mouse_buttons = buttons;

    if (dx || dy)
        mouse_emit(KNX_MOUSE_MOTION);
    if (changed & 0x07) {
        uint8_t pressed = (uint8_t)(buttons & changed);
        mouse_emit(pressed ? KNX_MOUSE_PRESS : KNX_MOUSE_RELEASE);
    }
}
extern uint64_t *knx_schedule(uint64_t *frame);
extern int knx_syscall(uint64_t *frame);

static uint8_t set2_to_set1(uint8_t code) {
    static const uint8_t map[128] = {
        [0x08] = 0x01,
        [0x0d] = 0x0f,
        [0x16] = 0x02, [0x1e] = 0x03, [0x26] = 0x04, [0x25] = 0x05,
        [0x2e] = 0x06, [0x36] = 0x07, [0x3d] = 0x08, [0x3e] = 0x09,
        [0x46] = 0x0a, [0x45] = 0x0b, [0x4e] = 0x0c, [0x55] = 0x0d,
        [0x66] = 0x0e, [0x0e] = 0x29, [0x54] = 0x1a, [0x5b] = 0x1b,
        [0x5d] = 0x2b, [0x4c] = 0x27, [0x52] = 0x28, [0x41] = 0x33,
        [0x49] = 0x34, [0x4a] = 0x35, [0x29] = 0x39, [0x5a] = 0x1c,
        [0x12] = 0x2a, [0x59] = 0x36, [0x14] = 0x1d, [0x11] = 0x38,
        [0x1c] = 0x1e, [0x32] = 0x30, [0x21] = 0x2e, [0x23] = 0x20,
        [0x24] = 0x12, [0x2b] = 0x21, [0x34] = 0x22, [0x33] = 0x23,
        [0x43] = 0x17, [0x3b] = 0x24, [0x42] = 0x25, [0x4b] = 0x26,
        [0x3a] = 0x32, [0x31] = 0x31, [0x44] = 0x18, [0x4d] = 0x19,
        [0x15] = 0x10, [0x2d] = 0x13, [0x1b] = 0x1f, [0x2c] = 0x14,
        [0x3c] = 0x16, [0x2a] = 0x2f, [0x1d] = 0x11, [0x22] = 0x2d,
        [0x35] = 0x15, [0x1a] = 0x2c, [0x71] = 0x53,
    };
    return code < 128 ? map[code] : 0;
}

static int ps2_raw_set2;
static int ps2_break_next;

static uint8_t ps2_last_make;
static int ps2_make_pending;
static uint64_t ps2_make_tick;

#ifdef KNX_PS2_DEBUG
static void ps2_debug(const char *label, uint8_t code) {
    static const char hex[] = "0123456789ABCDEF";
    knx_puts("[PS2 ");
    knx_puts(label);
    knx_puts("] 0x");
    knx_putc(hex[(code >> 4) & 15]);
    knx_putc(hex[code & 15]);
    knx_puts("\n");
}
#else
#define ps2_debug(label, code) ((void)0)
#endif

static void ps2_emit_event(uint8_t code, int pressed) {
    struct key_event ev;
    ev.code = code;
    ev.pressed = (uint8_t)pressed;
    ev.mods = key_mods;
    if (key_ev_write - key_ev_read < 128) {
        key_events[key_ev_write & 127] = ev;
        ++key_ev_write;
    }
}

static void ps2_key_byte(uint8_t sc) {
    uint8_t code;
    int release = 0;

    if (sc == 0xf0) {
        ps2_raw_set2 = 1;
        ps2_break_next = 1;
        ps2_debug("break-prefix", sc);
        return;
    }
    if (ps2_break_next) {
        ps2_break_next = 0;
        code = ps2_raw_set2 ? set2_to_set1(sc) : (uint8_t)(sc & 0x7f);
        if (!code)
            return;
        release = 1;
    } else if (sc == 0xe0) {
        key_ev_ext = 1;
        ps2_debug("ext-prefix", sc);
        return;
    } else {
        code = ps2_raw_set2 ? set2_to_set1(sc) : sc;
        if (!code)
            return;
        if (!ps2_raw_set2 && (code & 0x80)) {
            release = 1;
            code &= 0x7f;
        }
    }

    if (key_ev_ext) {
        key_ev_ext = 0;
        if (code == 0x38)
            code = (uint8_t)(code | 0x80);
        else {
            ps2_debug("ext-drop", code);
            return;
        }
    }

    if (release) {
        if (code == ps2_last_make) {
            ps2_make_pending = 0;
            ps2_last_make = 0;
        }
        ps2_debug("break", code);
        if (code == 0x2a || code == 0x36)
            key_mods &= ~KNX_KEYMOD_SHIFT;
        else if (code == 0x1d)
            key_mods &= ~KNX_KEYMOD_CTRL;
        else if (code == 0x38 || code == 0xb8)
            key_mods &= ~KNX_KEYMOD_ALT;
        ps2_emit_event(code, 0);
        return;
    }

    if (ps2_make_pending && code == ps2_last_make &&
        knx_ticks() - ps2_make_tick < 5) {
        ps2_debug("duplicate", code);
        return;
    }
    ps2_make_pending = 1;
    ps2_last_make = code;
    ps2_make_tick = knx_ticks();
    ps2_debug("make", code);

    if (code == 0x53 &&
        (key_mods & (KNX_KEYMOD_CTRL | KNX_KEYMOD_ALT)) ==
            (KNX_KEYMOD_CTRL | KNX_KEYMOD_ALT))
        knx_warm_reboot();
    if (code < 128 && key_write - key_read < 128) {
        key_queue[key_write & 127] = code;
        ++key_write;
    }
    if (code == 0x2a || code == 0x36)
        key_mods |= KNX_KEYMOD_SHIFT;
    else if (code == 0x1d)
        key_mods |= KNX_KEYMOD_CTRL;
    else if (code == 0x38 || code == 0xb8)
        key_mods |= KNX_KEYMOD_ALT;
    ps2_emit_event(code, 1);
}

void knx_ps2_poll(void) {
    uint8_t status = inb(0x64);
    if (status == 0xff || !(status & 1))
        return;
    ps2_key_byte(inb(0x60));
}

uint64_t *knx_interrupt_dispatch(uint64_t vector, uint64_t error, uint64_t *frame) {
    if (vector < 32) {
        if (vector == 14 && knx_task_is_user_context()) {
            uint64_t fault_address = 0;
            __asm__ volatile("mov %%cr2,%0" : "=r"(fault_address));
            if (!knx_vm_handle_fault(fault_address, error))
                return frame;
        }
        if (knx_task_is_user_context()) {

            uint64_t fault_address = 0;
            if (vector == 14)
                __asm__ volatile("mov %%cr2,%0" : "=r"(fault_address));
            knx_puts("\nKNX user exception at rip=");
            knx_print_u64(frame[17]);
            knx_puts(" err=");
            knx_print_u64(error);
            if (vector == 14) {
                knx_puts(" cr2=");
                knx_print_u64(fault_address);
            }
            knx_puts("\n");
            knx_task_kill_current();
            return knx_schedule(frame);
        }
        static const char *names[32] = {"Divide error",
                                        "Debug",
                                        "NMI",
                                        "Breakpoint",
                                        "Overflow",
                                        "Bound range",
                                        "Invalid opcode",
                                        "Device unavailable",
                                        "Double fault",
                                        "Coprocessor overrun",
                                        "Invalid TSS",
                                        "Segment not present",
                                        "Stack fault",
                                        "General protection fault",
                                        "Page fault",
                                        "Reserved exception",
                                        "x87 floating point",
                                        "Alignment check",
                                        "Machine check",
                                        "SIMD floating point",
                                        "Virtualization",
                                        "Control protection",
                                        "Reserved exception",
                                        "Reserved exception",
                                        "Reserved exception",
                                        "Reserved exception",
                                        "Reserved exception",
                                        "Reserved exception",
                                        "Hypervisor injection",
                                        "VMM communication",
                                        "Security exception",
                                        "Reserved exception"};
        __asm__ volatile("cli");
        knx_puts("\nKNX exception: ");
        knx_puts(names[vector]);
        knx_puts(" (error 0x");
        static const char hex[] = "0123456789ABCDEF";
        for (int s = 60; s >= 0; s -= 4)
            knx_putc(hex[(error >> s) & 15]);
        knx_puts(")\nCR2: 0x");
        uint64_t cr2 = 0;
        if (vector == 14)
            __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
        for (int s = 60; s >= 0; s -= 4)
            knx_putc(hex[(cr2 >> s) & 15]);
        knx_puts("\nRIP: 0x");
        uint64_t rip = frame[17];
        for (int s = 60; s >= 0; s -= 4)
            knx_putc(hex[(rip >> s) & 15]);
        knx_puts("\nCS: 0x");
        for (int s = 60; s >= 0; s -= 4)
            knx_putc(hex[(frame[18] >> s) & 15]);
        knx_puts(" RSP: 0x");
        for (int s = 60; s >= 0; s -= 4)
            knx_putc(hex[(frame[20] >> s) & 15]);
        knx_task_debug_current();
        knx_puts("\nSystem halted.\n");
        for (;;)
            __asm__ volatile("hlt");
    }
    if (vector == 32) {
        if (knx_smp_is_bsp()) {
            __sync_fetch_and_add(&tick_count, 1);
            if (tick_count == 1000) {
                knx_puts("[MOUSE] poll bytes=");
                knx_print_u64(mouse_poll_bytes);
                knx_puts(" irqs=");
                knx_print_u64(mouse_irq_count);
                knx_puts("\n");
            }
            if (tick_count == 100) {
                knx_puts("[MOUSE] init status=");
                knx_print_u64(mouse_dbg);
                knx_puts(" irq=");
                knx_print_u64((uint64_t)mouse_irq_active);
                knx_puts(" irqs=");
                knx_print_u64(mouse_irq_count);
                knx_puts("\n");
            }

            if (mouse_irq_active && (inb(0x64) & 1u)) {
                (void)inb(0x60);
                ++mouse_poll_bytes;
            }
            knx_audio_tick();
            knx_net_tick();
            knx_pcspk_tick();
            knx_usb_tick();
            knx_i2c_tick();

            outb(0x20, 0x20);
            knx_smp_ap_timer_eoi();
        } else {
            knx_smp_ap_timer_eoi();
        }
        knx_irq_dispatch((unsigned)vector, frame);
        return knx_schedule(frame);
    }
    if (vector == 128) {
        if (knx_syscall(frame))
            return knx_schedule(frame);
        return frame;
    }
    if (vector == 33 && (inb(0x64) & 0x20)) {

        mouse_packet_byte(inb(0x60));
        if (vector >= 32 && vector < 48) {
            outb(0x20, 0x20);
            knx_irq_dispatch((unsigned)vector, frame);
        }
        return frame;
    }
    if (vector == 44) {

        mouse_packet_byte(inb(0x60));
    }
    if (vector == 44)
        ++mouse_irq_count;
    if (vector == 33)
        ps2_key_byte(inb(0x60));
    if (vector >= 40 && vector < 48)
        outb(0xa0, 0x20);
    if (vector >= 32 && vector < 48)
        outb(0x20, 0x20);
    if (vector >= 32 && vector < 48)
        knx_irq_dispatch((unsigned)vector, frame);
    return frame;
}
uint64_t knx_ticks(void) {
    return tick_count;
}
char knx_keyboard_read(void) {
    char usb = knx_usbkbd_read();
    if (usb)
        return usb;
    static const char map[128] = {
        [0x02] = '1',  [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',  [0x07] = '6',
        [0x08] = '7',  [0x09] = '8', [0x0a] = '9', [0x0b] = '0', [0x0c] = '-',  [0x0d] = '=',
        [0x0e] = '\b', [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',  [0x14] = 't',
        [0x15] = 'y',  [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',  [0x1c] = '\n',
        [0x1e] = 'a',  [0x1f] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',  [0x23] = 'h',
        [0x24] = 'j',  [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x2c] = 'z',
        [0x2d] = 'x',  [0x2e] = 'c', [0x2f] = 'v', [0x30] = 'b', [0x31] = 'n',  [0x32] = 'm',
        [0x33] = ',',  [0x34] = '.', [0x35] = '/', [0x39] = ' '};

    if (key_read == key_write)
        knx_ps2_poll();
    if (key_read == key_write)
        return 0;
    uint8_t sc = key_queue[key_read++ & 127];
    if (sc & 0x80 || sc >= 128)
        return 0;
    return map[sc];
}
int knx_key_event_read(uint8_t *code, uint8_t *pressed, uint16_t *mods) {
    if (key_ev_read == key_ev_write)
        return 0;
    struct key_event ev = key_events[key_ev_read & 127];
    ++key_ev_read;
    *code = ev.code;
    *pressed = ev.pressed;
    *mods = ev.mods;
    return 1;
}

void knx_key_event_emit(uint8_t code, uint8_t pressed) {

    if (pressed && code == 0x53 &&
        (key_mods & (KNX_KEYMOD_CTRL | KNX_KEYMOD_ALT)) ==
            (KNX_KEYMOD_CTRL | KNX_KEYMOD_ALT))
        knx_warm_reboot();
    if (key_ev_write - key_ev_read >= 128)
        return;
    struct key_event ev;
    ev.code = code;
    ev.pressed = pressed;
    ev.mods = key_mods;
    key_events[key_ev_write & 127] = ev;
    ++key_ev_write;
}
void knx_key_mods_set(uint16_t mods) {
    key_mods = mods;
}
void knx_interrupts_init(void) {
    int ps2_ready = i8042_keyboard_init();

    outb(0x20, 0x11);
    outb(0xa0, 0x11);
    outb(0x21, 0x20);
    outb(0xa1, 0x28);
    outb(0x21, 0x04);
    outb(0xa1, 0x02);
    outb(0x21, 0x01);
    outb(0xa1, 0x01);
    uint16_t irq_mask = knx_irq_enabled_mask();
    outb(0x21, (uint8_t)irq_mask);

    if (mouse_irq_active)
        irq_mask = (uint16_t)(irq_mask & ~(1u << 12));
    outb(0xa1, (uint8_t)(irq_mask >> 8));
    uint16_t divisor = 11932;
    outb(0x43, 0x36);
    outb(0x40, divisor & 255);
    outb(0x40, divisor >> 8);
    if (!ps2_ready)
        knx_log("[WARN]", "PS/2 keyboard not detected; use the serial console or a supported legacy keyboard");
    else
        knx_log("[INFO]", "PS/2 keyboard online (IRQ1 + poll fallback)");
    if (mouse_irq_active)
        knx_log("[INFO]", "PS/2 mouse online (IRQ12, 3-byte packets)");
    __asm__ volatile("sti");
}
void knx_load_idt(const struct idtr *p) {
    __asm__ volatile("lidt (%0)" ::"r"(p) : "memory");
}
