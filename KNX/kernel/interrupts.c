#include "knx.h"
#include "knx_arch.h"
#include "knx_syscall.h"

/*
 * AArch64 interrupt / exception layer.
 *
 * External interrupts arrive through a GICv2 (QEMU virt: GICD at 0x08000000,
 * GICC at 0x08010000) and the periodic tick comes from the AArch64 generic
 * timer PPI 30.  The assembly
 * vector table (kernel/interrupts.S) saves a 40-word frame and calls
 * knx_interrupt_dispatch() with the VBAR entry code, ESR_EL1 and that frame.
 */

static volatile uint64_t tick_count;

/* ---- GICv2 MMIO -------------------------------------------------------- */

#define KNX_GICD_BASE 0x08000000ULL
#define KNX_GICC_BASE 0x08010000ULL

#define GICD_CTLR       0x0000u
#define GICD_ISENABLER  0x0100u
#define GICD_ICENABLER  0x0180u
#define GICD_ICPENDR    0x0280u
#define GICD_IPRIORITYR 0x0400u
#define GICD_ITARGETSR  0x0800u
#define GICD_ICFGR      0x0c00u

#define GICC_CTLR 0x0000u
#define GICC_PMR  0x0004u
#define GICC_BPR  0x0008u
#define GICC_IAR  0x000cu
#define GICC_EOIR 0x0010u

static inline volatile uint32_t *gicd_reg(unsigned offset) {
    return (volatile uint32_t *)(uintptr_t)(KNX_GICD_BASE + offset);
}
static inline volatile uint32_t *gicc_reg(unsigned offset) {
    return (volatile uint32_t *)(uintptr_t)(KNX_GICC_BASE + offset);
}
static inline volatile uint8_t *gicd_byte(unsigned offset) {
    return (volatile uint8_t *)(uintptr_t)(KNX_GICD_BASE + offset);
}
static inline uint32_t gicd_read(unsigned offset) { return *gicd_reg(offset); }
static inline void gicd_write(unsigned offset, uint32_t value) { *gicd_reg(offset) = value; }
static inline uint32_t gicc_read(unsigned offset) { return *gicc_reg(offset); }
static inline void gicc_write(unsigned offset, uint32_t value) { *gicc_reg(offset) = value; }

/* ---- AArch64 generic timer -------------------------------------------- */

static inline void timer_cval_write(uint64_t value) {
    __asm__ volatile("msr cntv_cval_el0, %0" ::"r"(value) : "memory");
}
static inline void timer_ctl_write(uint64_t value) {
    __asm__ volatile("msr cntv_ctl_el0, %0" ::"r"(value) : "memory");
}

static uint64_t tick_interval(void) {
    uint64_t interval = knx_tsc_hz() / 100u; /* 100 Hz */
    return interval ? interval : 1u;
}

/* ---- IRQ handler table ------------------------------------------------- */

#define KNX_IRQ_TABLE_SIZE 128u
static knx_irq_handler_fn irq_handlers[KNX_IRQ_TABLE_SIZE];
static volatile uint16_t irq_registered_mask;

int knx_irq_register(unsigned vector, knx_irq_handler_fn handler) {
    if (vector > KNX_IRQ_TABLE_SIZE - 1u)
        vector = KNX_IRQ_TABLE_SIZE - 1u;
    if (!handler)
        return -1;

    irq_handlers[vector] = handler;

    unsigned word = vector / 32u;
    unsigned bit = vector % 32u;
    gicd_write(GICD_ISENABLER + word * 4u, 1u << bit);
    *gicd_byte(GICD_IPRIORITYR + vector) = 0xa0u;
    if (vector >= 16u) /* SPIs are routed to CPU 0; PPIs are per-CPU already */
        *gicd_byte(GICD_ITARGETSR + vector) = 0x01u;

    irq_registered_mask = (uint16_t)(irq_registered_mask | (uint16_t)(1u << (vector & 15u)));
    return 0;
}

void knx_irq_dispatch(unsigned vector, uint64_t *frame) {
    if (vector >= KNX_IRQ_TABLE_SIZE)
        return;
    knx_irq_handler_fn handler = irq_handlers[vector];
    if (handler)
        handler(vector, frame);
}

uint16_t knx_irq_enabled_mask(void) {
    return irq_registered_mask;
}

/* ---- GIC / timer initialisation --------------------------------------- */

void knx_exceptions_init(void) {
    knx_irq_disable();

    gicd_write(GICD_CTLR, 0u);
    gicc_write(GICC_CTLR, 0u);

    /* PPI 30 (architectural timer): medium priority, target this CPU,
     * level-sensitive. */
    *gicd_byte(GICD_IPRIORITYR + KNX_ARCH_TIMER_IRQ) = 0xa0u;
    *gicd_byte(GICD_ITARGETSR + KNX_ARCH_TIMER_IRQ) = 0x01u;
    unsigned cfg_off = GICD_ICFGR + (KNX_ARCH_TIMER_IRQ / 16u) * 4u;
    unsigned cfg_shift = (KNX_ARCH_TIMER_IRQ % 16u) * 2u;
    uint32_t cfg = gicd_read(cfg_off);
    cfg = (uint32_t)((cfg & ~(3u << cfg_shift)) | (0u << cfg_shift)); /* level-sensitive */
    gicd_write(cfg_off, cfg);

    gicd_write(GICD_ICPENDR + (KNX_ARCH_TIMER_IRQ / 32u) * 4u,
               1u << (KNX_ARCH_TIMER_IRQ % 32u));
    gicd_write(GICD_ISENABLER + (KNX_ARCH_TIMER_IRQ / 32u) * 4u,
               1u << (KNX_ARCH_TIMER_IRQ % 32u));

    /* Accept all priorities and enable both GIC interfaces. */
    gicc_write(GICC_PMR, 0xffu);
    gicc_write(GICC_BPR, 0u);
    knx_dmb();
    gicc_write(GICC_CTLR, 1u);
    gicd_write(GICD_CTLR, 1u);
    knx_dsb();
    knx_isb();
}

void knx_interrupts_init(void) {
    timer_cval_write(knx_read_tsc() + tick_interval());
    timer_ctl_write(1u); /* ENABLE=1, IMASK=0 */
    knx_isb();
    knx_irq_enable();
}

/* ---- Exception dispatch ------------------------------------------------ */

extern uint64_t *knx_schedule(uint64_t *frame);

static void print_hex64(uint64_t value) {
    static const char hex[] = "0123456789ABCDEF";
    for (int shift = 60; shift >= 0; shift -= 4)
        knx_putc(hex[(value >> shift) & 15u]);
}

static void kernel_panic(uint64_t vector, uint64_t esr, uint64_t *frame) {
    knx_irq_disable();
    knx_puts("\nKNX kernel synchronous exception (vector ");
    knx_print_u64(vector);
    knx_puts(", esr=0x");
    print_hex64(esr);
    knx_puts(")\n PC=0x");
    print_hex64(frame[KNX_FRAME_PC]);
    knx_puts(" FAR=0x");
    print_hex64(frame[KNX_FRAME_FAR]);
    knx_puts(" SP=0x");
    print_hex64(frame[KNX_FRAME_SP]);
    knx_puts("\n");
    knx_task_debug_current();
    knx_puts("System halted.\n");
    for (;;)
        knx_halt();
}

static uint64_t *handle_irq(uint64_t *frame) {
    uint32_t iar = gicc_read(GICC_IAR);
    unsigned irqid = (unsigned)(iar & 0x3ffu);

    if (irqid == KNX_ARCH_TIMER_IRQ) {
        __sync_fetch_and_add(&tick_count, 1);
        timer_cval_write(knx_read_tsc() + tick_interval()); /* re-arm one-shot compare */
        knx_audio_tick();
        knx_net_tick();
        knx_usb_tick();
        knx_i2c_tick();
        knx_irq_dispatch(irqid, frame);
        gicc_write(GICC_EOIR, iar);
        knx_irq_enable();
        return knx_schedule(frame);
    }

    if (irqid < 0x3ffu)
        knx_irq_dispatch(irqid, frame);
    gicc_write(GICC_EOIR, iar);
    knx_irq_enable();
    return frame;
}

static uint64_t *handle_lower_sync(uint64_t esr, uint64_t *frame) {
    unsigned ec = (unsigned)((esr >> 26) & 0x3fu);

    if (ec == 0x15u) { /* SVC from AArch64 EL0 */
        frame[KNX_FRAME_VECTOR] = 128u;
        if (knx_syscall(frame))
            return knx_schedule(frame);
        return frame;
    }

    if (ec == 0x20u || ec == 0x21u) { /* instruction / data abort from EL0 */
        uint64_t far = frame[KNX_FRAME_FAR];
        if (!knx_vm_handle_fault(far, esr))
            return frame;
        knx_puts("\nKNX user abort PC=0x");
        print_hex64(frame[KNX_FRAME_PC]);
        knx_puts(" FAR=0x");
        print_hex64(far);
        knx_puts(" ESR=0x");
        print_hex64(esr);
        knx_puts("\n");
        knx_task_kill_current();
        return knx_schedule(frame);
    }

    /* Any other EL0 synchronous exception is fatal for the task only. */
    knx_puts("\nKNX user exception PC=0x");
    print_hex64(frame[KNX_FRAME_PC]);
    knx_puts(" ESR=0x");
    print_hex64(esr);
    knx_puts("\n");
    knx_task_kill_current();
    return knx_schedule(frame);
}

uint64_t *knx_interrupt_dispatch(uint64_t vector, uint64_t error, uint64_t *frame) {
    if (!frame)
        return frame;

    switch (vector) {
    case 1u:
    case 5u:
    case 9u:
    case 13u: /* IRQ (current EL SPx, lower EL AArch64, SP0 / AArch32 variants) */
        return handle_irq(frame);
    case 8u:
    case 12u: /* synchronous from lower EL (AArch64 / AArch32) */
        return handle_lower_sync(error, frame);
    default:
        /* 0/4: sync current EL; everything else (FIQ, SError, reserved). */
        kernel_panic(vector, error, frame);
    }
    return frame; /* not reached */
}

uint64_t knx_ticks(void) {
    return tick_count;
}

void knx_warm_reboot(void) {
    /* PSCI SYSTEM_RESET (32-bit call).  On success this never returns. */
    register uint64_t x0 __asm__("x0") = 0x84000009ULL;
    __asm__ volatile("hvc #0" : "+r"(x0) : : "memory");
    knx_irq_disable();
    for (;;)
        knx_halt();
}

/* ---- Keyboard / mouse event queues ------------------------------------ *
 * Input arrives from USB HID, which calls knx_key_event_emit() and
 * knx_mouse_event_emit().  The rings and scancode tables are kept so the
 * existing consumers keep working.
 */

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

#define MOUSE_RING 64u
static volatile struct knx_mouse_event mouse_events[MOUSE_RING];
static volatile uint32_t mouse_ev_read, mouse_ev_write;
static volatile int32_t mouse_x = -1, mouse_y = -1;
static volatile uint8_t mouse_buttons;

/* Set 2 -> set 1 scancode translation (retained for HID/PS/2 compatibility). */
static const uint8_t ps2_set2_to_set1[128] __attribute__((unused)) = {
    [0x08] = 0x01, [0x0d] = 0x0f,
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

int knx_mouse_event_read(struct knx_mouse_event *event) {
    if (mouse_ev_read == mouse_ev_write)
        return 0;
    if (event)
        *event = mouse_events[mouse_ev_read & (MOUSE_RING - 1u)];
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
    mouse_events[mouse_ev_write & (MOUSE_RING - 1u)] = ev;
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
    uint8_t mask = buttons & 0x07u;
    uint8_t changed = (uint8_t)(mouse_buttons ^ mask);
    mouse_buttons = mask;
    if (dx || dy)
        mouse_emit(KNX_MOUSE_MOTION);
    if (changed & 0x07u)
        mouse_emit((uint8_t)((mask & changed) ? KNX_MOUSE_PRESS : KNX_MOUSE_RELEASE));
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
    uint8_t sc = key_queue[key_read++ & 127u];
    if (sc & 0x80u || sc >= 128u)
        return 0;
    return map[sc];
}

int knx_key_event_read(uint8_t *code, uint8_t *pressed, uint16_t *mods) {
    if (key_ev_read == key_ev_write)
        return 0;
    struct key_event ev = key_events[key_ev_read & 127u];
    ++key_ev_read;
    *code = ev.code;
    *pressed = ev.pressed;
    *mods = ev.mods;
    return 1;
}

void knx_key_event_emit(uint8_t code, uint8_t pressed) {
    if (pressed && code == 0x53u &&
        (key_mods & (KNX_KEYMOD_CTRL | KNX_KEYMOD_ALT)) ==
            (KNX_KEYMOD_CTRL | KNX_KEYMOD_ALT))
        knx_warm_reboot();
    if (key_ev_write - key_ev_read >= 128u)
        return;
    struct key_event ev;
    ev.code = code;
    ev.pressed = pressed;
    ev.mods = key_mods;
    key_events[key_ev_write & 127u] = ev;
    ++key_ev_write;
}

void knx_key_mods_set(uint16_t mods) {
    key_mods = mods;
}

/* No legacy PS/2 controller exists on AArch64; input is driven by USB HID. */
void knx_ps2_poll(void) {
}
