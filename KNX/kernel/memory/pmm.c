#include "knx.h"

/*
 * Physical allocator for AArch64/QEMU "virt".  The boot memory map comes from
 * the UEFI loader, and the regions used by firmware, the flight log and the
 * loaded kernel image are reserved explicitly below.
 */
#define MAX_PHYS_BYTES (64ULL * 1024 * 1024 * 1024)
#define PAGE_SIZE 4096ULL
#define MAX_PAGES (MAX_PHYS_BYTES / PAGE_SIZE)
#define BITMAP_BYTES (MAX_PAGES / 8)
#define KNX_PMM_MIN_PHYS 0x40000000ULL
#define KNX_PMM_MIN_PAGE (KNX_PMM_MIN_PHYS / PAGE_SIZE)
static uint8_t page_bitmap[BITMAP_BYTES];
static uint64_t free_pages;
static uint64_t total_pages;
static volatile unsigned page_lock;
static uint8_t heap[64 * 1024] __attribute__((aligned(16)));
typedef struct heap_block heap_block;
struct __attribute__((aligned(16))) heap_block {
    size_t size;
    heap_block *prev, *next;
    uint64_t magic;
    uint32_t free;
};
#define HEAP_BLOCK_MAGIC 0x4b4e584845415031ULL
static heap_block *heap_head;
static volatile unsigned heap_lock;

static void heap_acquire(void) {
    while (__sync_lock_test_and_set(&heap_lock, 1))
        knx_pause();
}

static void heap_release(void) { __sync_lock_release(&heap_lock); }

static void reserve_physical(uint64_t start, uint64_t end) {
    uint64_t first = start / PAGE_SIZE;
    uint64_t last = (end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t p = first; p < last && p < MAX_PAGES; ++p) {
        if (!(page_bitmap[p >> 3] & (1u << (p & 7)))) {
            page_bitmap[p >> 3] |= 1u << (p & 7);
            if (free_pages)
                --free_pages;
        }
    }
}

void knx_memory_init(knx_boot_info *info) {
    for (uint64_t i = 0; i < BITMAP_BYTES; ++i)
        page_bitmap[i] = 0xff;
    total_pages = 0;
    free_pages = 0;
    if (!info || info->magic != KNX_BOOT_MAGIC || !info->memory_map)
        return;
    for (uint64_t i = 0; i < info->memory_region_count; ++i) {
        knx_memory_region *r = &info->memory_map[i];
        if (!r->length || r->base >= MAX_PHYS_BYTES)
            continue;
        uint64_t end = r->base + r->length;
        if (end < r->base || end > MAX_PHYS_BYTES)
            end = MAX_PHYS_BYTES;
        uint64_t first = (r->base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t last = end / PAGE_SIZE;
        if (last > MAX_PAGES)
            last = MAX_PAGES;
        if (r->type == KNX_MEMORY_USABLE) {
            for (uint64_t p = first; p < last; ++p) {
                page_bitmap[p >> 3] &= (uint8_t)~(1u << (p & 7));
                ++free_pages;
            }
            total_pages += last - first;
        }
    }

    /* Firmware and the UEFI loader live in the first 8 MiB of DRAM. */
    reserve_physical(0x40000000ULL, 0x40080000ULL);
    /* Flight-log ring and the low part of the kernel image window. */
    reserve_physical(KNX_FLIGHT_PHYS, 0x40090000ULL);
    /* The kernel is loaded at 0x40080000 (virtual == physical). */
    uint64_t kernel_start = 0x40080000ULL / PAGE_SIZE;
    uint64_t kernel_end =
        ((uint64_t)(uintptr_t)&__kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t p = kernel_start; p < kernel_end && p < MAX_PAGES; ++p) {
        if (!(page_bitmap[p >> 3] & (1u << (p & 7)))) {
            page_bitmap[p >> 3] |= 1u << (p & 7);
            if (free_pages)
                --free_pages;
        }
    }

    /* UEFI-allocated boot metadata and the in-memory root image must not be
     * handed out as heap pages later in the boot. */
    if (info) {
        reserve_physical((uint64_t)(uintptr_t)info,
                         (uint64_t)(uintptr_t)info + sizeof(*info));
        if (info->memory_map && info->memory_region_count)
            reserve_physical((uint64_t)(uintptr_t)info->memory_map,
                             (uint64_t)(uintptr_t)info->memory_map +
                                 info->memory_region_count * sizeof(knx_memory_region));
        if (info->initrd_base && info->initrd_size)
            reserve_physical(info->initrd_base, info->initrd_base + info->initrd_size);
    }

    info->total_memory = total_pages * PAGE_SIZE;
    info->available_memory = free_pages * PAGE_SIZE;
}

void *knx_alloc_page(void) {
    while (__sync_lock_test_and_set(&page_lock, 1))
        knx_pause();
    for (uint64_t p = KNX_PMM_MIN_PAGE; p < MAX_PAGES; ++p) {
        uint8_t mask = (uint8_t)(1u << (p & 7));
        if (!(page_bitmap[p >> 3] & mask)) {
            page_bitmap[p >> 3] |= mask;
            if (free_pages)
                --free_pages;
            void *page = (void *)(uintptr_t)(p * PAGE_SIZE);
            __sync_lock_release(&page_lock);
            return page;
        }
    }
    __sync_lock_release(&page_lock);
    return 0;
}

void *knx_alloc_contiguous_pages(size_t count) {
    if (!count)
        return 0;
    while (__sync_lock_test_and_set(&page_lock, 1))
        knx_pause();
    uint64_t start = 0, run = 0;
    for (uint64_t p = KNX_PMM_MIN_PAGE; p < MAX_PAGES; ++p) {
        uint8_t mask = (uint8_t)(1u << (p & 7));
        if (page_bitmap[p >> 3] & mask) {
            run = 0;
            continue;
        }
        if (!run)
            start = p;
        if (++run == count) {
            for (uint64_t q = start; q < start + count; ++q) {
                page_bitmap[q >> 3] |= (uint8_t)(1u << (q & 7));
                if (free_pages)
                    --free_pages;
            }
            __sync_lock_release(&page_lock);
            return (void *)(uintptr_t)(start * PAGE_SIZE);
        }
    }
    __sync_lock_release(&page_lock);
    return 0;
}

void knx_free_page(void *ptr) {
    uint64_t p = (uintptr_t)ptr / PAGE_SIZE;
    if ((uintptr_t)ptr % PAGE_SIZE || p < KNX_PMM_MIN_PAGE || p >= MAX_PAGES)
        return;
    while (__sync_lock_test_and_set(&page_lock, 1))
        knx_pause();
    uint8_t mask = (uint8_t)(1u << (p & 7));
    if (page_bitmap[p >> 3] & mask) {
        page_bitmap[p >> 3] &= (uint8_t)~mask;
        ++free_pages;
    }
    __sync_lock_release(&page_lock);
}

void *knx_heap_alloc(size_t size) {
    if (!size || size > SIZE_MAX - 15u)
        return 0;
    size = (size + 15u) & ~(size_t)15u;
    if (size > sizeof(heap) - sizeof(heap_block))
        return 0;
    heap_acquire();
    if (!heap_head) {
        heap_head = (heap_block *)heap;
        heap_head->size = sizeof(heap) - sizeof(*heap_head);
        heap_head->prev = heap_head->next = 0;
        heap_head->magic = HEAP_BLOCK_MAGIC;
        heap_head->free = 1;
    }
    heap_block *block = heap_head;
    while (block && (!block->free || block->size < size))
        block = block->next;
    if (!block) {
        heap_release();
        return 0;
    }
    if (block->size >= size + sizeof(heap_block) + 16u) {
        heap_block *split = (heap_block *)((uint8_t *)(block + 1) + size);
        split->size = block->size - size - sizeof(*split);
        split->prev = block;
        split->next = block->next;
        split->magic = HEAP_BLOCK_MAGIC;
        split->free = 1;
        if (split->next)
            split->next->prev = split;
        block->next = split;
        block->size = size;
    }
    block->free = 0;
    void *result = block + 1;
    heap_release();
    return result;
}

void knx_heap_free(void *ptr) {
    if (!ptr)
        return;
    heap_acquire();
    heap_block *block = heap_head;
    while (block && (void *)(block + 1) != ptr)
        block = block->next;
    if (!block || block->magic != HEAP_BLOCK_MAGIC || block->free) {
        heap_release();
        return;
    }
    uint8_t *bytes = ptr;
    for (size_t i = 0; i < block->size; ++i)
        bytes[i] = 0;
    block->free = 1;
    if (block->next && block->next->free && block->next->magic == HEAP_BLOCK_MAGIC) {
        heap_block *next = block->next;
        block->size += sizeof(*next) + next->size;
        block->next = next->next;
        if (block->next)
            block->next->prev = block;
    }
    if (block->prev && block->prev->free && block->prev->magic == HEAP_BLOCK_MAGIC) {
        heap_block *prev = block->prev;
        prev->size += sizeof(*block) + block->size;
        prev->next = block->next;
        if (prev->next)
            prev->next->prev = prev;
    }
    heap_release();
}

int knx_heap_selftest(void) {
    uint8_t *first = knx_heap_alloc(128);
    uint8_t *second = knx_heap_alloc(256);
    if (!first || !second) {
        knx_heap_free(first);
        knx_heap_free(second);
        return 0;
    }
    for (unsigned i = 0; i < 128; ++i)
        first[i] = 0xa5;
    knx_heap_free(first);
    knx_heap_free(first);
    uint8_t *reused = knx_heap_alloc(128);
    int ok = reused == first;
    if (reused)
        for (unsigned i = 0; i < 128; ++i)
            if (reused[i] != 0)
                ok = 0;
    knx_heap_free(reused);
    knx_heap_free(second);
    return ok;
}

uint64_t knx_free_memory(void) {
    while (__sync_lock_test_and_set(&page_lock, 1))
        knx_pause();
    uint64_t bytes = free_pages * PAGE_SIZE;
    __sync_lock_release(&page_lock);
    return bytes;
}

uint64_t knx_total_memory(void) {
    return total_pages * PAGE_SIZE;
}
