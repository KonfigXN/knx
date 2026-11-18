#include "knx.h"

/*
 * KFS parsing is retained as a fallback filesystem, but the ATA/IDE PIO
 * controller (ports 0x1f0/0x170/0x3f6) does not exist on AArch64.  The only
 * disk-read hook is a stub that reports "no media"; the primary root is the
 * UFS image the UEFI loader passes in memory (see fs_ufs.c / knx_ufs_memdisk).
 */

#define FS_MAX_FILES 64
#define SECTOR_SIZE 512

struct fs_entry {
    char name[40];
    uint32_t first_sector;
    uint32_t size;
    uint32_t sectors;
    uint32_t flags;
    uint8_t reserved[8];
};
static struct fs_entry directory[FS_MAX_FILES];
static unsigned file_count;
static int mounted;

static int kfs_disk_read(uint32_t lba, uint8_t *buffer) {
    (void)lba;
    (void)buffer;
    return 0; /* No legacy IDE disk on AArch64. */
}

static int same_name(const char *a, const char *b) {
    unsigned i = 0;
    while (a[i] && b[i]) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return 0;
        ++i;
    }
    return a[i] == b[i];
}

static struct knx_vnode kfs_vnodes[FS_MAX_FILES];
static const struct knx_vfs_ops kfs_vfs_ops;
static struct knx_vnode kfs_root_vnode;

static int kfs_lookup(struct knx_vnode *node, const char *name, struct knx_vnode **out) {
    if (!node || !node->ops || !node->ops->lookup) return -1;
    if (node->type != KNX_VFS_DIRECTORY) return -1;
    if (!name) return -1;
    const char *leaf = name;
    if (same_name(name, "etc") || same_name(name,"sys") || same_name(name, "bin")) {
        static struct knx_vnode etc_node = { .ops = &kfs_vfs_ops, .type = KNX_VFS_DIRECTORY };
        etc_node.inode = same_name(name,"etc") ? 0x455443 :
                         same_name(name,"sys") ? 0x535953 : 0x42494e;
        if (out)
            *out = &etc_node;
        return 0;
    }
    if (node->inode == 0x455443 && same_name(name, "motd"))
        leaf = "MOTD";
    if (node->inode == 0x535953 && same_name(name,"ext")) {
        static struct knx_vnode ext_node={.ops=&kfs_vfs_ops,.type=KNX_VFS_DIRECTORY,.inode=0x455854};
        if (out)
            *out = &ext_node;
        return 0;
    }
    if (node->inode == 0x455854) leaf="HELLO.KEXT";
    if (node->inode == 0x42494e && same_name(name, "init")) leaf = "INIT.ELF";
    for (unsigned i = 0; i < file_count; ++i) {
        if (same_name(leaf, directory[i].name)) {
            struct knx_vnode *child = &kfs_vnodes[i];
            child->ops = &kfs_vfs_ops;
            child->private_data = &directory[i];
            child->inode = (uint64_t)(i + 1);
            child->type = KNX_VFS_REGULAR;
            if (out) *out = child;
            return 0;
        }
    }
    return -1;
}

static int kfs_readdir(struct knx_vnode *node, uint64_t index, struct knx_vfs_dirent *entry) {
    if (!node || !node->ops || !node->ops->readdir) return -1;
    if (node->type != KNX_VFS_DIRECTORY) return -1;
    if (node->inode == 0x455443) {
        if (index || !file_count) return -1;
        entry->name[0]='m'; entry->name[1]='o'; entry->name[2]='t'; entry->name[3]='d'; entry->name[4]=0;
        entry->inode=1; entry->type=KNX_VFS_REGULAR; return 0;
    }
    if (node->inode == 0x42494e) {
        if (index) return -1;
        entry->name[0] = 'i'; entry->name[1] = 'n'; entry->name[2] = 'i';
        entry->name[3] = 't'; entry->name[4] = 0;
        entry->inode = 3; entry->type = KNX_VFS_REGULAR;
        return 0;
    }
    if (index >= (uint64_t)file_count) return -1;
    const struct fs_entry *ent = &directory[index];
    unsigned len = 0;
    while (ent->name[len] && len + 1 < sizeof(ent->name)) ++len;
    for (unsigned i = 0; i <= len; ++i) entry->name[i] = ent->name[i];
    entry->name[len] = 0;
    entry->inode = (uint64_t)(index + 1);
    entry->type = KNX_VFS_REGULAR;
    return 0;
}

static int kfs_stat(struct knx_vnode *node, struct knx_vfs_stat *stat) {
    if (!node || !node->ops || !node->ops->stat) return -1;
    if (node->type == KNX_VFS_DIRECTORY) {
        stat->inode = 0;
        stat->size = (uint64_t)file_count;
        stat->type = KNX_VFS_DIRECTORY;
        stat->mode = 0755;
        return 0;
    }
    const struct fs_entry *ent = (const struct fs_entry *)node->private_data;
    if (!ent) return -1;
    stat->inode = node->inode;
    stat->size = ent->size;
    stat->type = KNX_VFS_REGULAR;
    stat->mode = 0644;
    return 0;
}

static int64_t kfs_read(struct knx_vnode *node, uint64_t offset, void *buffer, uint64_t count) {
    if (!node || !node->ops || !node->ops->read) return -1;
    if (node->type != KNX_VFS_REGULAR) return -1;
    const struct fs_entry *ent = (const struct fs_entry *)node->private_data;
    if (!ent || !buffer) return -1;
    if (offset > (uint64_t)ent->size || count > (uint64_t)ent->size - offset) return -2;
    uint8_t sector[SECTOR_SIZE] __attribute__((aligned(16)));
    uint8_t *dst = (uint8_t *)buffer;
    uint64_t done = 0;
    while (done < count) {
        uint32_t lba = ent->first_sector + (uint32_t)(done / SECTOR_SIZE);
        if (!kfs_disk_read(lba, sector)) return -1;
        uint32_t skip = (uint32_t)(done % SECTOR_SIZE);
        uint32_t take = SECTOR_SIZE - skip;
        if ((uint64_t)take > count - done) take = (uint32_t)(count - done);
        for (uint32_t j = 0; j < take; ++j) dst[j] = sector[skip + j];
        dst += take;
        done += take;
    }
    return (int64_t)count;
}

static int kfs_fs_mount(void *device, void **fs_data, struct knx_vnode **root) {
    (void)device;
    *fs_data = 0;
    *root = &kfs_root_vnode;
    return 0;
}

static int kfs_fs_unmount(void *fs_data) {
    (void)fs_data;
    return 0;
}

static const struct knx_vfs_ops kfs_vfs_ops = {
    .mount = kfs_fs_mount,
    .unmount = kfs_fs_unmount,
    .lookup = kfs_lookup,
    .read = kfs_read,
    .readdir = kfs_readdir,
    .stat = kfs_stat,
};

static struct knx_vnode kfs_root_vnode = { .ops = &kfs_vfs_ops, .private_data = 0, .inode = 0, .type = KNX_VFS_DIRECTORY };

int knx_root_mount_test(void) {
    struct knx_vnode *node = 0;
    struct knx_vfs_stat st;
    char contents[64];
    static const char expected[] = "KNX test root filesystem";

    if (knx_vfs_resolve("/etc/motd", &node) || !node) return 0;
    if (knx_vfs_stat(node, &st)) return 0;
    unsigned len = sizeof(expected) - 1;
    if (st.size < (uint64_t)len + 1) return 0;
    uint64_t want = st.size < (uint64_t)len + 2 ? st.size : (uint64_t)len + 2;
    int64_t n = knx_vfs_read(node, 0, contents, want);
    if (n != (int64_t)want) return 0;
    for (unsigned i = 0; i < len; ++i)
        if (contents[i] != expected[i]) return 0;
    if (contents[len] == '\n') return 1;
    if (want == (uint64_t)len + 2 && contents[len] == '\r' && contents[len + 1] == '\n')
        return 1;
    return 0;
}

void knx_kfs_init(void) {
    uint8_t sector[SECTOR_SIZE] __attribute__((aligned(16)));
    for (unsigned i = 0; i < SECTOR_SIZE; ++i)
        sector[i] = 0;
    mounted = 0;

    if (!kfs_disk_read(0, sector)) {
        knx_log("[KFS]", "No IDE disk on AArch64; UFS initrd is the primary root");
        return;
    }

    if (sector[510] != 0x4b || sector[511] != 0x58 ||
        sector[0] != 'K' || sector[1] != 'N' || sector[2] != 'X' || sector[3] != 'F' ||
        sector[4] != 'S' || sector[5] != '0' || sector[6] != '1')
        return;

    file_count = *(uint32_t *)(sector + 8);
    if (file_count > FS_MAX_FILES)
        return;
    unsigned copied = 0;
    int valid = 1;
    for (uint32_t lba = 1; lba < 9 && copied < file_count; ++lba) {
        if (!kfs_disk_read(lba, sector)) {
            valid = 0;
            break;
        }
        for (unsigned i = 0; i < SECTOR_SIZE / sizeof(struct fs_entry) && copied < file_count;
             ++i, ++copied) {
            directory[copied] = ((struct fs_entry *)sector)[i];
            if (directory[copied].name[39] != 0 || directory[copied].first_sector < 16 ||
                directory[copied].sectors > 2048 ||
                directory[copied].size > directory[copied].sectors * SECTOR_SIZE ||
                directory[copied].first_sector >= (1u << 28) ||
                directory[copied].sectors > (1u << 28) - directory[copied].first_sector) {
                valid = 0;
                break;
            }
        }
        if (!valid)
            break;
    }
    if (!valid || copied != file_count)
        return;

    mounted = 1;
    (void)knx_vfs_mount("/kfs", "kfs", &kfs_vfs_ops, 0);
    knx_puts("KFS mounted at /kfs (fallback)\n");
}

int knx_kfs_ready(void) {
    return mounted;
}

void knx_kfs_list(void) {
    if (!mounted) {
        knx_puts("No KFS data disk found. AArch64 uses the UFS initrd as its root.\n");
        return;
    }
    for (unsigned i = 0; i < file_count; ++i) {
        knx_puts(directory[i].name);
        knx_puts("  ");
        knx_print_u64(directory[i].size);
        knx_puts(" bytes\n");
    }
}

int knx_kfs_cat(const char *name) {
    if (!mounted)
        return -1;
    const struct fs_entry *entry = 0;
    for (unsigned i = 0; i < file_count; ++i)
        if (same_name(name, directory[i].name)) {
            entry = &directory[i];
            break;
        }
    if (!entry || entry->sectors > 2048 || entry->size > entry->sectors * SECTOR_SIZE ||
        entry->first_sector < 16 || entry->first_sector >= (1u << 28) ||
        entry->sectors > (1u << 28) - entry->first_sector)
        return 0;
    uint8_t sector[SECTOR_SIZE] __attribute__((aligned(16)));
    uint32_t remaining = entry->size;
    uint8_t last = '\n';
    for (uint32_t i = 0; i < entry->sectors && remaining; ++i) {
        if (!kfs_disk_read(entry->first_sector + i, sector))
            return 0;
        uint32_t take = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
        for (uint32_t j = 0; j < take; ++j) {
            knx_putc((char)sector[j]);
            last = sector[j];
        }
        remaining -= take;
    }
    if (remaining)
        return 0;
    if (last != '\n')
        knx_puts("\n");
    return 1;
}

int64_t knx_kfs_read(const char *name, void *buffer, uint64_t capacity) {
    if (!mounted || !name || (!buffer && capacity))
        return -1;
    const struct fs_entry *entry = 0;
    for (unsigned i = 0; i < file_count; ++i)
        if (same_name(name, directory[i].name)) {
            entry = &directory[i];
            break;
        }
    if (!entry || entry->sectors > 2048 || entry->size > entry->sectors * SECTOR_SIZE ||
        entry->first_sector < 16 || entry->first_sector >= (1u << 28) ||
        entry->sectors > (1u << 28) - entry->first_sector)
        return -1;
    if (entry->size > capacity)
        return -2;
    uint8_t sector[SECTOR_SIZE] __attribute__((aligned(16)));
    uint32_t remaining = entry->size;
    uint8_t *dst = buffer;
    for (uint32_t i = 0; i < entry->sectors && remaining; ++i) {
        if (!kfs_disk_read(entry->first_sector + i, sector))
            return -1;
        uint32_t take = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
        for (uint32_t j = 0; j < take; ++j)
            dst[j] = sector[j];
        dst += take;
        remaining -= take;
    }
    return remaining ? -1 : (int64_t)entry->size;
}
