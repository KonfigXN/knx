#pragma once
#include <stddef.h>
#include <stdint.h>

#define KNX_VFS_NAME_MAX 64
#define KNX_VFS_MOUNTS_MAX 16
#define KNX_VFS_PATH_MAX 256
#define KNX_VFS_EINVAL (-1)
#define KNX_VFS_ENAMETOOLONG (-2)
#define KNX_VFS_READ 1u
#define KNX_VFS_WRITE 2u
#define KNX_VFS_DIRECTORY 1u
#define KNX_VFS_REGULAR 2u
#define KNX_VFS_SYMLINK 3u

struct knx_vnode;
struct knx_vfs_stat { uint64_t inode, size; uint32_t type, mode; };
struct knx_vfs_dirent { char name[KNX_VFS_NAME_MAX]; uint64_t inode; uint32_t type; };
struct knx_vfs_ops {
    int (*mount)(void *device, void **fs_data, struct knx_vnode **root);
    int (*unmount)(void *fs_data);
    int (*lookup)(struct knx_vnode *, const char *, struct knx_vnode **);
    int64_t (*read)(struct knx_vnode *, uint64_t, void *, uint64_t);
    int (*readdir)(struct knx_vnode *, uint64_t, struct knx_vfs_dirent *);
    int (*stat)(struct knx_vnode *, struct knx_vfs_stat *);
};
struct knx_vnode { const struct knx_vfs_ops *ops; void *private_data; uint64_t inode; uint32_t type; };
struct knx_vfs_file { struct knx_vnode *node; uint64_t offset; uint32_t flags; };

#ifdef __cplusplus
extern "C" {
#endif
void knx_vfs_init(void);
void knx_vfs_selftest(void);
int knx_vfs_normalize(const char *path, char *out, unsigned outsz);
int knx_vfs_mount(const char *path, const char *type, const struct knx_vfs_ops *ops, void *device);
int knx_vfs_unmount(const char *path);
int knx_vfs_resolve(const char *path, struct knx_vnode **out);
int64_t knx_vfs_read(struct knx_vnode *node, uint64_t offset, void *buffer, uint64_t count);
int knx_vfs_readdir(struct knx_vnode *node, uint64_t index, struct knx_vfs_dirent *entry);
int knx_vfs_stat(struct knx_vnode *node, struct knx_vfs_stat *stat);
#ifdef __cplusplus
}
#endif
