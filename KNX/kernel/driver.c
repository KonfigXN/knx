#include "knx.h"

/*
 * PCI driver registry.
 *
 * Interrupt handling (knx_irq_register/knx_irq_dispatch) belongs to the GIC
 * layer in interrupts.c, along with the tick and input state, so this
 * translation unit only owns the driver registry.
 */

#define MAX_DRIVERS 16

typedef struct {
    const char *name;
    uint8_t class_code, subclass, prog_if;
    knx_driver_probe_fn probe;
} knx_driver_entry;

static knx_driver_entry drivers[MAX_DRIVERS];
static unsigned driver_count;

static int same_text(const char *a, const char *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

int knx_driver_register(const char *name, uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                        knx_driver_probe_fn probe) {
    if (!name || !probe || driver_count >= MAX_DRIVERS)
        return -1;
    for (unsigned i = 0; i < driver_count; ++i)
        if (same_text(drivers[i].name, name))
            return -1;
    drivers[driver_count++] =
        (knx_driver_entry){name, class_code, subclass, prog_if, probe};
    return 0;
}

/*
 * Registry query used by the PCIe scanner in device.c.  Matching is done here
 * so that the registry representation stays private to this file.
 */
int knx_driver_match(const knx_pci_device *pci, const char **name_out) {
    if (!pci)
        return 0;
    for (unsigned i = 0; i < driver_count; ++i) {
        const knx_driver_entry *d = &drivers[i];
        if ((d->class_code == 0xff || d->class_code == pci->class_code) &&
            (d->subclass == 0xff || d->subclass == pci->subclass) &&
            (d->prog_if == 0xff || d->prog_if == pci->prog_if) && d->probe(pci)) {
            if (name_out)
                *name_out = d->name;
            return 1;
        }
    }
    return 0;
}
