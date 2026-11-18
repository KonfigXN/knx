/*
 * AArch64 fallback for the two small helpers that kernel/rust.rs used to
 * provide.  There is no aarch64-unknown-none Rust core sysroot in this
 * toolchain, so rust.rs is no longer compiled.  These C implementations keep
 * the exported ABI (knx.h) intact until an AArch64 Rust core is available;
 * replace this file with the Rust object once that sysroot exists.
 */
#include <stdint.h>

uint64_t knx_rust_value(void) { return 0x4b4e5821ULL; } /* "KNX!" */

uint64_t knx_rust_add(uint64_t a, uint64_t b) { return a + b; }
