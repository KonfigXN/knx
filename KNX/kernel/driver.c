#include "knx.h"

#define IRQ_HANDLERS_PER_VECTOR 4
static knx_irq_handler_fn irq_handlers[16][IRQ_HANDLERS_PER_VECTOR];
static volatile uint16_t enabled_irq_mask = 0xfffc;

int knx_irq_register(unsigned vector, knx_irq_handler_fn handler) {
    if (vector < 32 || vector >= 48 || !handler)
        return -1;
    unsigned slot = vector - 32;
    int inserted = 0;
    for (unsigned i = 0; i < IRQ_HANDLERS_PER_VECTOR; ++i) {
        knx_irq_handler_fn current = __atomic_load_n(&irq_handlers[slot][i], __ATOMIC_ACQUIRE);
        if (current == handler)
            return 0;
        if (current)
            continue;
        knx_irq_handler_fn expected = 0;
        if (__atomic_compare_exchange_n(&irq_handlers[slot][i], &expected, handler, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            inserted = 1;
            break;
        }
    }
    if (!inserted)
        return -1;
    unsigned irq = vector - 32;
    __atomic_fetch_and(&enabled_irq_mask, (uint16_t)~(1u << irq), __ATOMIC_RELEASE);
    uint16_t mask_port = irq < 8 ? 0x21 : 0xa1;
    unsigned bit = irq & 7;
    knx_io_out8(mask_port, (uint8_t)(knx_io_in8(mask_port) & ~(1u << bit)));
    if (irq >= 8)
        knx_io_out8(0x21, (uint8_t)(knx_io_in8(0x21) & ~(1u << 2)));
    return 0;
}

uint16_t knx_irq_enabled_mask(void) {
    return __atomic_load_n(&enabled_irq_mask, __ATOMIC_ACQUIRE);
}

void knx_irq_dispatch(unsigned vector, uint64_t *frame) {
    if (vector < 32 || vector >= 48)
        return;
    for (unsigned i = 0; i < IRQ_HANDLERS_PER_VECTOR; ++i) {
        knx_irq_handler_fn handler =
            __atomic_load_n(&irq_handlers[vector - 32][i], __ATOMIC_ACQUIRE);
        if (handler)
            handler(vector, frame);
    }
}
