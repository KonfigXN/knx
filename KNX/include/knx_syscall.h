#pragma once
#include <stdint.h>

enum {
    KNX_SYS_YIELD = 0,
    KNX_SYS_WRITE = 1,
    KNX_SYS_GETPID = 2,
    KNX_SYS_GETTID = 3,
    KNX_SYS_TICKS = 4,
    KNX_SYS_FORK = 5,
    KNX_SYS_EXIT = 6,
    KNX_SYS_THREAD_CREATE = 7,
    KNX_SYS_WAITPID = 8,
    KNX_SYS_THREAD_JOIN = 9,
    KNX_SYS_MMAP = 10,
    KNX_SYS_MUNMAP = 11,
    KNX_SYS_EXEC = 12,
    KNX_SYS_EXECVE = 13,

    KNX_SYS_OPEN = 14,
    KNX_SYS_READ = 15,
    KNX_SYS_CLOSE = 16,
    KNX_SYS_READLINE = 18,
    KNX_SYS_FB_MAP = 19,
    KNX_SYS_FB_FLUSH = 20,

    KNX_SYS_MMAP_SHARED = 21,

    KNX_SYS_CONSOLE_GFX = 22,

    KNX_SYS_PCM_WRITE = 23,

    KNX_SYS_SOCKET = 24,
    KNX_SYS_BIND = 25,
    KNX_SYS_SENDTO = 26,
    KNX_SYS_RECVFROM = 27,
    KNX_SYS_CLOSE_FD = 28,

    KNX_SYS_IFCONFIG = 29,
    KNX_SYS_NETSTAT = 30,
    KNX_SYS_NET_INFO = 31,
    KNX_SYS_PING = 32,

    KNX_SYS_READKEY = 33,
    KNX_SYS_MOUSE = 34,
    KNX_SYS_SPAWN = 35,
    KNX_SYS_READPIPE = 36,
    KNX_SYS_BOOTFLAGS = 37,

    KNX_SYS_REBOOT = 38,

    KNX_SYS_GETLOG = 39
};

struct knx_key_event {
    uint8_t keycode;
    uint8_t pressed;
    uint16_t modifiers;
};

#define KNX_KEYMOD_SHIFT 0x0001
#define KNX_KEYMOD_CTRL 0x0002
#define KNX_KEYMOD_ALT 0x0004

static inline __attribute__((always_inline)) int64_t knx_syscall2(uint64_t number, uint64_t arg0,
                                                                  uint64_t arg1) {
    register uint64_t x8 __asm__("x8") = number;
    register uint64_t x0 __asm__("x0") = arg0;
    register uint64_t x1 __asm__("x1") = arg1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return (int64_t)x0;
}

static inline __attribute__((always_inline)) int64_t knx_syscall3(uint64_t number, uint64_t arg0,
                                                                  uint64_t arg1, uint64_t arg2) {
    register uint64_t x8 __asm__("x8") = number;
    register uint64_t x0 __asm__("x0") = arg0;
    register uint64_t x1 __asm__("x1") = arg1;
    register uint64_t x2 __asm__("x2") = arg2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return (int64_t)x0;
}

static inline __attribute__((always_inline)) int64_t knx_sys_waitpid(int64_t pid,
                                                                     int32_t *status,
                                                                     uint64_t options) {
    return knx_syscall3(8, (uint64_t)pid, (uint64_t)(uintptr_t)status, options);
}

static inline __attribute__((always_inline)) int64_t knx_sys_yield(void) {
    return knx_syscall2(KNX_SYS_YIELD, 0, 0);
}

static inline __attribute__((always_inline)) int64_t knx_sys_write(const char *buffer,
                                                                   uint64_t length) {
    return knx_syscall2(KNX_SYS_WRITE, (uint64_t)(uintptr_t)buffer, length);
}

static inline __attribute__((always_inline)) int64_t knx_sys_getpid(void) {
    return knx_syscall2(KNX_SYS_GETPID, 0, 0);
}

static inline __attribute__((always_inline)) int64_t knx_sys_gettid(void) {
    return knx_syscall2(KNX_SYS_GETTID, 0, 0);
}

static inline __attribute__((always_inline)) int64_t knx_sys_ticks(void) {
    return knx_syscall2(KNX_SYS_TICKS, 0, 0);
}

static inline __attribute__((always_inline)) int64_t knx_sys_fork(void) {
    return knx_syscall2(KNX_SYS_FORK, 0, 0);
}

static inline __attribute__((always_inline, noreturn)) void knx_sys_exit(int status) {
    (void)knx_syscall2(KNX_SYS_EXIT, (uint64_t)(int64_t)status, 0);
    for (;;)
        __asm__ volatile("yield");
}

static inline __attribute__((always_inline)) int64_t knx_sys_thread_create(void (*entry)(void)) {
    return knx_syscall2(KNX_SYS_THREAD_CREATE, (uint64_t)(uintptr_t)entry, 0);
}

static inline __attribute__((always_inline)) int64_t knx_sys_exec(const void *image, uint64_t size) {
    return knx_syscall2(KNX_SYS_EXEC, (uint64_t)(uintptr_t)image, size);
}

static inline __attribute__((always_inline)) int64_t knx_sys_open(const char *path) {
    return knx_syscall2(KNX_SYS_OPEN, (uint64_t)(uintptr_t)path, 0);
}
static inline __attribute__((always_inline)) int64_t knx_sys_read(int64_t fd,
                                                                void *buffer, uint64_t count) {
    return knx_syscall3(KNX_SYS_READ, (uint64_t)(int64_t)fd,
                        (uint64_t)(uintptr_t)buffer, count);
}
static inline __attribute__((always_inline)) int64_t knx_sys_close(int64_t fd) {
    return knx_syscall2(KNX_SYS_CLOSE, (uint64_t)(int64_t)fd, 0);
}

static inline __attribute__((always_inline)) int64_t knx_sys_pcm_write(const void *buffer,
                                                                      uint64_t count) {
    return knx_syscall2(KNX_SYS_PCM_WRITE, (uint64_t)(uintptr_t)buffer, count);
}

static inline __attribute__((always_inline)) int64_t knx_sys_readline(char *buffer, uint64_t max) {
    return knx_syscall2(KNX_SYS_READLINE, (uint64_t)(uintptr_t)buffer, max);
}

struct knx_fb_info {
    uint64_t addr;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    uint32_t masks[4];
};

static inline __attribute__((always_inline)) int64_t knx_sys_fb_map(struct knx_fb_info *info) {
    return knx_syscall2(KNX_SYS_FB_MAP, (uint64_t)(uintptr_t)info, 0);
}

static inline __attribute__((always_inline)) int64_t knx_sys_fb_flush(uint64_t addr, uint64_t size) {
    return knx_syscall2(KNX_SYS_FB_FLUSH, addr, size);
}

static inline __attribute__((always_inline)) int64_t knx_sys_mmap_shared(uint64_t flags) {
    return knx_syscall2(KNX_SYS_MMAP_SHARED, flags, 0);
}

static inline __attribute__((always_inline)) int64_t knx_sys_console_gfx(uint64_t flags) {
    return knx_syscall2(KNX_SYS_CONSOLE_GFX, flags, 0);
}

static inline __attribute__((always_inline)) int64_t knx_sys_execve(const char *path,
                                                                    const char *const argv[],
                                                                    const char *const envp[]) {
    register uint64_t x8 __asm__("x8") = KNX_SYS_EXECVE;
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)path;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)argv;
    register uint64_t x2 __asm__("x2") = (uint64_t)(uintptr_t)envp;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return (int64_t)x0;
}

static inline __attribute__((always_inline)) int64_t knx_syscall6(uint64_t number, uint64_t arg0,
                                                                  uint64_t arg1, uint64_t arg2,
                                                                  uint64_t arg3, uint64_t arg4,
                                                                  uint64_t arg5) {
    register uint64_t x8 __asm__("x8") = number;
    register uint64_t x0 __asm__("x0") = arg0;
    register uint64_t x1 __asm__("x1") = arg1;
    register uint64_t x2 __asm__("x2") = arg2;
    register uint64_t x3 __asm__("x3") = arg3;
    register uint64_t x4 __asm__("x4") = arg4;
    register uint64_t x5 __asm__("x5") = arg5;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    return (int64_t)x0;
}

static inline __attribute__((always_inline)) int64_t knx_sys_socket(int domain, int type, int protocol) {
    return knx_syscall3(KNX_SYS_SOCKET, (uint64_t)domain, (uint64_t)type, (uint64_t)protocol);
}

static inline __attribute__((always_inline)) int64_t knx_sys_bind(int fd, uint32_t addr, uint16_t port) {
    return knx_syscall3(KNX_SYS_BIND, (uint64_t)fd, (uint64_t)addr, (uint64_t)port);
}

static inline __attribute__((always_inline)) int64_t knx_sys_sendto(int fd, const void *buf, uint64_t len,
                                                                    uint64_t flags, uint32_t addr,
                                                                    uint16_t port) {
    return knx_syscall6(KNX_SYS_SENDTO, (uint64_t)fd, (uint64_t)(uintptr_t)buf, len, flags,
                        (uint64_t)addr, (uint64_t)port);
}

static inline __attribute__((always_inline)) int64_t knx_sys_recvfrom(int fd, void *buf, uint64_t len,
                                                                      uint64_t flags, uint32_t *addr,
                                                                      uint16_t *port) {
    return knx_syscall6(KNX_SYS_RECVFROM, (uint64_t)fd, (uint64_t)(uintptr_t)buf, len, flags,
                        (uint64_t)(uintptr_t)addr, (uint64_t)(uintptr_t)port);
}

static inline __attribute__((always_inline)) int64_t knx_sys_close_fd(int fd) {
    return knx_syscall2(KNX_SYS_CLOSE_FD, (uint64_t)fd, 0);
}

static inline __attribute__((always_inline)) int64_t knx_sys_ifconfig(void) {
    return knx_syscall2(KNX_SYS_IFCONFIG, 0, 0);
}

static inline __attribute__((always_inline)) int64_t knx_sys_netstat(void) {
    return knx_syscall2(KNX_SYS_NETSTAT, 0, 0);
}

static inline __attribute__((always_inline)) int64_t knx_sys_net_info(void) {
    return knx_syscall2(KNX_SYS_NET_INFO, 0, 0);
}

static inline __attribute__((always_inline)) int64_t knx_sys_ping(uint32_t target_hostip) {
    return knx_syscall2(KNX_SYS_PING, (uint64_t)target_hostip, 0);
}

static inline __attribute__((always_inline)) uint64_t knx_sys_bootflags(void) {
    register uint64_t x8 __asm__("x8") = KNX_SYS_BOOTFLAGS;
    register uint64_t x0 __asm__("x0") = 0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline __attribute__((always_inline)) void knx_sys_reboot(void) {
    register uint64_t x8 __asm__("x8") = KNX_SYS_REBOOT;
    register uint64_t x0 __asm__("x0") = 0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
}

static inline __attribute__((always_inline)) int64_t
knx_sys_getlog(uint8_t *buf, uint64_t max) {
    return knx_syscall2(KNX_SYS_GETLOG, (uint64_t)(uintptr_t)buf, max);
}

static inline __attribute__((always_inline)) int64_t
knx_sys_readkey(struct knx_key_event *event, uint64_t timeout_ticks) {
    return knx_syscall2(KNX_SYS_READKEY, (uint64_t)(uintptr_t)event, timeout_ticks);
}

struct knx_mouse_event {
    int32_t x;
    int32_t y;
    uint8_t buttons;
    uint8_t type;
    uint16_t reserved;
};

#define KNX_MOUSE_BUTTON_LEFT 0x01
#define KNX_MOUSE_BUTTON_RIGHT 0x02
#define KNX_MOUSE_BUTTON_MIDDLE 0x04

#define KNX_MOUSE_MOTION 0
#define KNX_MOUSE_PRESS 1
#define KNX_MOUSE_RELEASE 2

static inline __attribute__((always_inline)) int64_t
knx_sys_mouse(struct knx_mouse_event *event) {
    return knx_syscall2(KNX_SYS_MOUSE, (uint64_t)(uintptr_t)event, 0);
}

static inline __attribute__((always_inline)) int64_t
knx_sys_spawn(const char *path, char *const *argv, uint64_t flags) {
    return knx_syscall3(KNX_SYS_SPAWN, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)argv,
                        flags);
}

static inline __attribute__((always_inline)) int64_t
knx_sys_readpipe(int64_t pid, char *buffer, uint64_t max) {
    return knx_syscall3(KNX_SYS_READPIPE, (uint64_t)pid, (uint64_t)(uintptr_t)buffer, max);
}
