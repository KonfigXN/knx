#pragma once
#include <stdint.h>

#define KNX_EXT_MAGIC 0x315458454e584bULL
#define KNX_EXT_ABI_VERSION 1u
#define KNX_EXT_NAME_MAX 32u

typedef struct knx_ext_api {
    uint32_t abi_version;
    uint32_t struct_size;
    void (*kprintf)(const char *message);
    void *(*kmalloc)(uint64_t size);
    void (*kfree)(void *pointer);
    uint32_t (*pci_read)(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
    int (*irq_register)(unsigned vector, void (*handler)(unsigned, uint64_t *));
} knx_ext_api_t;

typedef struct knx_ext_header {
    uint64_t magic;
    uint32_t header_size;
    uint32_t abi_version;
    uint32_t version;
    uint32_t reserved;
    char name[KNX_EXT_NAME_MAX];
    char init_symbol[32];
    char exit_symbol[32];
} knx_ext_header_t;

typedef int (*knx_ext_init_fn)(const knx_ext_api_t *api);
typedef void (*knx_ext_exit_fn)(const knx_ext_api_t *api);
