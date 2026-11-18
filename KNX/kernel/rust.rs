#![no_std]

#[no_mangle]
pub extern "C" fn knx_rust_value() -> u64 {
    42
}

#[no_mangle]
pub extern "C" fn knx_rust_add(a: u64, b: u64) -> u64 {
    a.wrapping_add(b)
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}
