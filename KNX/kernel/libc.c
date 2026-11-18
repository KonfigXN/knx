#include "knx.h"

void *memset(void *destination, int value, size_t count) {
    uint8_t *dst = destination;
    while (count--)
        *dst++ = (uint8_t)value;
    return destination;
}

void *memcpy(void *destination, const void *source, size_t count) {
    uint8_t *dst = destination;
    const uint8_t *src = source;
    while (count--)
        *dst++ = *src++;
    return destination;
}
