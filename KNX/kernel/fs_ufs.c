#include "knx.h"
#include "ufs.h"

/*
 * UFS1 reader.  The ATA/IDE PIO path (ports 0x1f0/0x170/0x3f6) is gone: there
 * is no such controller on AArch64.  The UEFI loader supplies the root image
 * in memory via knx_boot_info.initrd_base, which main.c registers with
 * knx_ufs_memdisk(); that path is fully preserved below.
 */

#define UFS_SECTOR 512
#define UFS_FSB 4096
#define UFS_SECTORS_PER_FSB (UFS_FSB / UFS_SECTOR)
#define UFS_MAX_VNODES 32

static uint32_t ufs_part_lba;
static int ufs_mounted;
static const uint8_t *ufs_memdisk;
static uint64_t ufs_memdisk_size;

void knx_ufs_memdisk(const void *base, uint64_t size) {
    ufs_memdisk = base;
    ufs_memdisk_size = size;
}

static struct {
    int32_t sblkno;
    int32_t cblkno;
    int32_t iblkno;
    int32_t dblkno;
    uint32_t ncg;
    int32_t bsize;
    int32_t fsize;
    int32_t frag;
    uint32_t inopb;
    uint32_t ipg;
    int32_t fpg;
    uint32_t nindir;
    uint32_t magic;
} ufs_sb;

/* The only backing store is the in-memory image handed over by the loader. */
static int ufs_read_sector(uint32_t lba, uint8_t *buffer) {
    if (!ufs_memdisk)
        return 0;
    uint64_t offset = (uint64_t)lba * UFS_SECTOR;
    if (offset + UFS_SECTOR > ufs_memdisk_size)
        return 0;
    const uint8_t *src = ufs_memdisk + offset;
    for (unsigned i = 0; i < UFS_SECTOR; ++i)
        buffer[i] = src[i];
    return 1;
}

static int u_read_fsb(int32_t fsb, uint8_t *buf) {
    if (fsb < 0 || (uint32_t)fsb >= (uint32_t)ufs_sb.fpg)
        return 0;
    uint32_t start = ufs_part_lba + (uint32_t)fsb * UFS_SECTORS_PER_FSB;
    for (unsigned i = 0; i < UFS_SECTORS_PER_FSB; ++i)
        if (!ufs_read_sector(start + i, buf + i * UFS_SECTOR))
            return 0;
    return 1;
}

static uint32_t rd32(const uint8_t *p, unsigned off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) |
           ((uint32_t)p[off + 3] << 24);
}

#define UFS_BLOCK_POOL 4
static uint8_t ufs_blocks[UFS_BLOCK_POOL][UFS_FSB] __attribute__((aligned(16)));
static volatile unsigned ufs_block_next;

static uint8_t *ufs_block_get(void) {
    unsigned index = __sync_fetch_and_add(&ufs_block_next, 1) % UFS_BLOCK_POOL;
    return ufs_blocks[index];
}

static int u_read_inode(uint32_t ino, struct ufs1_dinode *out) {
    uint8_t *block = ufs_block_get();
    if (ino >= ufs_sb.ipg)
        return 0;
    unsigned inopb = ufs_sb.inopb ? ufs_sb.inopb : (UFS_FSB / sizeof(struct ufs1_dinode));
    uint32_t inode_fsb = (uint32_t)ufs_sb.iblkno + (ino / inopb);
    if (!u_read_fsb((int32_t)inode_fsb, block))
        return 0;
    uint8_t *src = block + (ino % inopb) * sizeof(struct ufs1_dinode);
    uint8_t *dst = (uint8_t *)out;
    for (unsigned i = 0; i < sizeof(*out); ++i)
        dst[i] = src[i];
    return 1;
}

struct ufs_vnode_private {
    uint32_t ino;
};
static struct knx_vnode ufs_vnodes[UFS_MAX_VNODES];
static struct ufs_vnode_private ufs_priv[UFS_MAX_VNODES];
static const struct knx_vfs_ops ufs_vfs_ops;
static struct knx_vnode ufs_root_vnode;

static struct ufs_vnode_private ufs_root_priv;

static int u_casefold(char c) {
    if (c >= 'a' && c <= 'z')
        return c - 32;
    return c;
}
static int u_name_eq(const char *a, const unsigned char *b, unsigned blen) {
    unsigned i = 0;
    while (a[i]) {
        if (i >= blen)
            return 0;
        if (u_casefold(a[i]) != u_casefold((char)b[i]))
            return 0;
        ++i;
    }
    return i == blen;
}

static int u_name_eq_elf(const char *a, const unsigned char *b, unsigned blen) {
    static const char suffix[] = ".ELF";
    unsigned alen = 0;
    while (a[alen])
        ++alen;
    if (blen != alen + 4)
        return 0;
    for (unsigned i = 0; i < alen; ++i)
        if (u_casefold(a[i]) != u_casefold((char)b[i]))
            return 0;
    for (unsigned i = 0; i < 4; ++i)
        if (u_casefold((char)b[alen + i]) != suffix[i])
            return 0;
    return 1;
}

static struct knx_vnode *u_alloc_vnode(uint32_t ino, uint32_t type) {
    for (unsigned i = 0; i < UFS_MAX_VNODES; ++i) {
        if (ufs_vnodes[i].ops == &ufs_vfs_ops && ufs_priv[i].ino == ino) {
            ufs_vnodes[i].type = type;
            return &ufs_vnodes[i];
        }
    }
    for (unsigned i = 0; i < UFS_MAX_VNODES; ++i) {
        if (ufs_vnodes[i].ops != &ufs_vfs_ops) {
            ufs_vnodes[i].ops = &ufs_vfs_ops;
            ufs_vnodes[i].inode = ino;
            ufs_vnodes[i].type = type;
            ufs_priv[i].ino = ino;
            ufs_vnodes[i].private_data = &ufs_priv[i];
            return &ufs_vnodes[i];
        }
    }
    return 0;
}

static uint32_t u_vnode_ino(struct knx_vnode *node) {
    if (!node)
        return 0;
    if (node->private_data)
        return ((struct ufs_vnode_private *)node->private_data)->ino;
    return (uint32_t)node->inode;
}

static int u_dir_lookup_in_chunk(uint8_t *buf, const char *name, uint32_t *found,
                                 int allow_elf) {
    unsigned off = 0;
    while (off + 8 <= UFS_DIRBLKSIZ) {
        uint32_t d_ino = rd32(buf, off);
        unsigned d_reclen = (unsigned)buf[off + 4] | ((unsigned)buf[off + 5] << 8);
        unsigned d_namlen = buf[off + 7];
        if (d_reclen == 0 || d_reclen > UFS_DIRBLKSIZ || off + d_reclen > UFS_DIRBLKSIZ)
            return 0;
        if (d_ino != 0 && d_namlen > 0 && d_namlen <= UFS_MAXNAMLEN) {
            if (u_name_eq(name, buf + off + 8, d_namlen)) {
                *found = d_ino;
                return 1;
            }
            if (allow_elf && u_name_eq_elf(name, buf + off + 8, d_namlen)) {
                *found = d_ino;
                return 1;
            }
        }
        off += d_reclen;
    }
    return 0;
}

static int ufs_lookup(struct knx_vnode *node, const char *name,
                      struct knx_vnode **out) {
    if (!node || !node->ops || !node->ops->lookup || !name || !out)
        return -1;
    if (node->type != KNX_VFS_DIRECTORY)
        return -1;
    uint32_t dir_ino = u_vnode_ino(node);
    struct ufs1_dinode din;
    if (!u_read_inode(dir_ino, &din))
        return -1;
    if ((din.di_mode & UFS_IFMT) != UFS_IFDIR)
        return -1;

    int32_t dir_fsb = din.di_db[0];
    if (dir_fsb == 0)
        return -1;
    uint8_t *block = ufs_block_get();
    if (!u_read_fsb(dir_fsb, block))
        return -1;
    uint32_t found = 0;

    for (unsigned pass = 0; pass < 2 && !found; ++pass) {
        for (unsigned chunk = 0; chunk < UFS_FSB / UFS_DIRBLKSIZ; ++chunk) {
            if (u_dir_lookup_in_chunk(block + chunk * UFS_DIRBLKSIZ, name, &found,
                                      pass == 1))
                break;
        }
    }
    if (!found)
        return -1;
    struct ufs1_dinode child;
    if (!u_read_inode(found, &child))
        return -1;
    uint32_t type = ((child.di_mode & UFS_IFMT) == UFS_IFDIR) ? KNX_VFS_DIRECTORY
                                                               : KNX_VFS_REGULAR;
    struct knx_vnode *v = u_alloc_vnode(found, type);
    if (!v)
        return -1;
    *out = v;
    return 0;
}

static int ufs_readdir(struct knx_vnode *node, uint64_t index,
                       struct knx_vfs_dirent *entry) {
    if (!node || !node->ops || !node->ops->readdir || !entry)
        return -1;
    if (node->type != KNX_VFS_DIRECTORY)
        return -1;
    uint32_t dir_ino = u_vnode_ino(node);
    struct ufs1_dinode din;
    if (!u_read_inode(dir_ino, &din))
        return -1;
    if ((din.di_mode & UFS_IFMT) != UFS_IFDIR)
        return -1;
    int32_t dir_fsb = din.di_db[0];
    if (dir_fsb == 0)
        return -1;
    uint8_t *block = ufs_block_get();
    if (!u_read_fsb(dir_fsb, block))
        return -1;
    uint64_t seen = 0;
    for (unsigned chunk = 0; chunk < UFS_FSB / UFS_DIRBLKSIZ; ++chunk) {
        uint8_t *base = block + chunk * UFS_DIRBLKSIZ;
        unsigned off = 0;
        while (off + 8 <= UFS_DIRBLKSIZ) {
            uint32_t d_ino = rd32(base, off);
            unsigned d_reclen =
                (unsigned)base[off + 4] | ((unsigned)base[off + 5] << 8);
            unsigned d_namlen = base[off + 7];
            unsigned d_type = base[off + 6];
            if (d_reclen == 0 || d_reclen > UFS_DIRBLKSIZ ||
                off + d_reclen > UFS_DIRBLKSIZ)
                break;
            if (d_ino != 0 && d_namlen > 0 && d_namlen <= UFS_MAXNAMLEN) {
                if (seen == index) {
                    for (unsigned i = 0; i < d_namlen && i + 1 < KNX_VFS_NAME_MAX; ++i)
                        entry->name[i] = (char)base[off + 8 + i];
                    entry->name[d_namlen < KNX_VFS_NAME_MAX ? d_namlen : KNX_VFS_NAME_MAX - 1] = 0;
                    entry->inode = d_ino;
                    entry->type = (d_type == UFS_DT_DIR) ? KNX_VFS_DIRECTORY
                                                         : KNX_VFS_REGULAR;
                    return 0;
                }
                ++seen;
            }
            off += d_reclen;
        }
    }
    return -1;
}

static int ufs_stat(struct knx_vnode *node, struct knx_vfs_stat *stat) {
    if (!node || !node->ops || !node->ops->stat || !stat)
        return -1;
    uint32_t ino = u_vnode_ino(node);
    struct ufs1_dinode din;
    if (!u_read_inode(ino, &din))
        return -1;
    stat->inode = ino;
    stat->size = din.di_size;
    stat->type = ((din.di_mode & UFS_IFMT) == UFS_IFDIR) ? KNX_VFS_DIRECTORY
                                                          : KNX_VFS_REGULAR;
    stat->mode = (uint32_t)(din.di_mode & 0777);
    if (!stat->mode)
        stat->mode = (stat->type == KNX_VFS_DIRECTORY) ? 0755 : 0644;
    return 0;
}

static int u_bmap(const struct ufs1_dinode *din, uint64_t lbn, int32_t *out) {
    uint64_t nind = ufs_sb.nindir ? ufs_sb.nindir : UFS_NINDIR;
    if (lbn < UFS_NDADDR) {
        *out = din->di_db[lbn];
        return *out ? 0 : -1;
    }
    lbn -= UFS_NDADDR;
    if (lbn < nind) {
        uint8_t *ib = ufs_block_get();
        if (!u_read_fsb(din->di_ib[0], ib))
            return -1;
        *out = (int32_t)rd32(ib, lbn * 4);
        return *out ? 0 : -1;
    }
    lbn -= nind;
    if (lbn < nind * nind) {
        uint64_t sub = lbn / nind;
        uint64_t idx = lbn % nind;
        uint8_t *dib = ufs_block_get();
        if (!u_read_fsb(din->di_ib[1], dib))
            return -1;
        int32_t first = (int32_t)rd32(dib, sub * 4);
        if (!first)
            return -1;
        uint8_t *fib = ufs_block_get();
        if (!u_read_fsb(first, fib))
            return -1;
        *out = (int32_t)rd32(fib, idx * 4);
        return *out ? 0 : -1;
    }
    return -1;
}

static int64_t ufs_read(struct knx_vnode *node, uint64_t offset, void *buffer,
                        uint64_t count) {
    if (!node || !node->ops || !node->ops->read || !buffer)
        return -1;
    if (node->type != KNX_VFS_REGULAR)
        return -1;
    uint32_t ino = u_vnode_ino(node);
    struct ufs1_dinode din;
    if (!u_read_inode(ino, &din))
        return -1;
    if (offset > din.di_size || count > din.di_size - offset)
        return -2;
    if (count == 0)
        return 0;
    uint8_t *dst = (uint8_t *)buffer;
    uint64_t done = 0;
    uint8_t *block = ufs_block_get();
    while (done < count) {
        uint64_t pos = offset + done;
        unsigned lbn = (unsigned)(pos / UFS_FSB);
        unsigned boff = (unsigned)(pos % UFS_FSB);
        int32_t fsb;
        if (u_bmap(&din, lbn, &fsb) != 0)
            return -1;
        if (!u_read_fsb(fsb, block))
            return -1;
        unsigned take = UFS_FSB - boff;
        if ((uint64_t)take > count - done)
            take = (unsigned)(count - done);
        for (unsigned i = 0; i < take; ++i)
            dst[i] = block[boff + i];
        dst += take;
        done += take;
    }
    return (int64_t)count;
}

static int ufs_fs_mount(void *device, void **fs_data, struct knx_vnode **root) {
    (void)device;
    *fs_data = 0;
    *root = &ufs_root_vnode;
    return 0;
}
static int ufs_fs_unmount(void *fs_data) {
    (void)fs_data;
    return 0;
}
static const struct knx_vfs_ops ufs_vfs_ops = {
    .mount = ufs_fs_mount,
    .unmount = ufs_fs_unmount,
    .lookup = ufs_lookup,
    .read = ufs_read,
    .readdir = ufs_readdir,
    .stat = ufs_stat,
};
static struct knx_vnode ufs_root_vnode = {
    .ops = &ufs_vfs_ops, .private_data = 0, .inode = UFS_ROOTINO, .type = KNX_VFS_DIRECTORY
};

static int u_parse_sb(const uint8_t *sb) {
    uint32_t magic = rd32(sb, UFS_FSOFF_MAGIC);
    if (magic != FS_UFS1_MAGIC)
        return 0;
    int32_t bsize = (int32_t)rd32(sb, UFS_FSOFF_BSIZE);
    int32_t fsize = (int32_t)rd32(sb, UFS_FSOFF_FSIZE);
    int32_t frag = (int32_t)rd32(sb, UFS_FSOFF_FRAG);
    uint32_t ncg = rd32(sb, UFS_FSOFF_NCG);
    uint32_t inopb = rd32(sb, UFS_FSOFF_INOPB);
    uint32_t ipg = rd32(sb, UFS_FSOFF_IPG);
    uint32_t nindir = rd32(sb, UFS_FSOFF_NINDIR);
    int32_t fpg = (int32_t)rd32(sb, UFS_FSOFF_CGA);
    int32_t sblkno = (int32_t)rd32(sb, UFS_FSOFF_SBLKNO);
    if (bsize != UFS_FSB || fsize != UFS_FSB || frag != 1)
        return 0;
    if (ncg != 1 || inopb == 0 || ipg == 0 || ipg > 1024 || fpg <= 0)
        return 0;
    if (nindir == 0 || nindir > UFS_NINDIR)
        return 0;
    if (sblkno != 2)
        return 0;
    if ((int32_t)rd32(sb, UFS_FSOFF_INODEFMT) != 2)
        return 0;
    return 1;
}
static void u_save_sb(const uint8_t *sb) {
    ufs_sb.sblkno = (int32_t)rd32(sb, UFS_FSOFF_SBLKNO);
    ufs_sb.cblkno = (int32_t)rd32(sb, UFS_FSOFF_CBLKNO);
    ufs_sb.iblkno = (int32_t)rd32(sb, UFS_FSOFF_IBLKNO);
    ufs_sb.dblkno = (int32_t)rd32(sb, UFS_FSOFF_DBLKNO);
    ufs_sb.ncg = rd32(sb, UFS_FSOFF_NCG);
    ufs_sb.bsize = (int32_t)rd32(sb, UFS_FSOFF_BSIZE);
    ufs_sb.fsize = (int32_t)rd32(sb, UFS_FSOFF_FSIZE);
    ufs_sb.frag = (int32_t)rd32(sb, UFS_FSOFF_FRAG);
    ufs_sb.inopb = rd32(sb, UFS_FSOFF_INOPB);
    ufs_sb.ipg = rd32(sb, UFS_FSOFF_IPG);
    ufs_sb.nindir = rd32(sb, UFS_FSOFF_NINDIR);
    ufs_sb.fpg = (int32_t)rd32(sb, UFS_FSOFF_CGA);
    ufs_sb.magic = rd32(sb, UFS_FSOFF_MAGIC);
}

static int u_try_partition(uint32_t part_lba, uint32_t part_sectors) {
    if (part_sectors < UFS_SBLOCK_SECTORS + 3)
        return 0;
    uint8_t sb[1536] __attribute__((aligned(16)));
    for (unsigned i = 0; i < 3; ++i)
        if (!ufs_read_sector(part_lba + UFS_SBLOCK_SECTORS + i, sb + i * UFS_SECTOR))
            return 0;
    if (!u_parse_sb(sb))
        return 0;

    uint32_t sb_size64_lo = rd32(sb, UFS_FSOFF_SIZE);
    if (sb_size64_lo == 0 || sb_size64_lo > 1024 * 1024)
        return 0;
    u_save_sb(sb);
    ufs_part_lba = part_lba;
    (void)part_sectors;
    return 1;
}

void knx_ufs_init(void) {
    ufs_mounted = 0;
    if (!ufs_memdisk) {
        knx_puts("UFS: no bootloader-supplied RAM image available\n");
        return;
    }
    uint32_t sectors = (uint32_t)(ufs_memdisk_size / UFS_SECTOR);
    knx_puts("UFS: bootloader-supplied RAM image, ");
    knx_print_u64(ufs_memdisk_size / 1024);
    knx_puts(" KiB\n");
    if (sectors < 64 || !u_try_partition(0, sectors)) {
        knx_puts("UFS: RAM image is not a UFS1 volume\n");
        return;
    }

    ufs_mounted = 1;
    for (unsigned i = 0; i < UFS_MAX_VNODES; ++i)
        ufs_vnodes[i].ops = 0;
    ufs_root_priv.ino = UFS_ROOTINO;
    ufs_root_vnode.private_data = &ufs_root_priv;
    ufs_root_vnode.inode = UFS_ROOTINO;
    ufs_root_vnode.type = KNX_VFS_DIRECTORY;
    ufs_root_vnode.ops = &ufs_vfs_ops;

    if (knx_vfs_mount("/", "ufs", &ufs_vfs_ops, 0) != 0) {
        ufs_mounted = 0;
        knx_puts("UFS not mounted, KFS is root (fallback)\n");
        return;
    }
    if (knx_vfs_mount("/ufs", "ufs", &ufs_vfs_ops, 0) != 0) {
        knx_vfs_unmount("/");
        ufs_mounted = 0;
        knx_puts("UFS not mounted, KFS is root (fallback)\n");
        return;
    }
    knx_puts("UFS mounted at / (read-only, primary)\n");
    knx_puts("UFS mounted at /ufs (read-only)\n");
}

int knx_ufs_ready(void) {
    return ufs_mounted;
}
