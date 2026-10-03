//! StackChan board drivers, ported from M5Stack's StackChan firmware and BSP
//! (MIT). Register-level only; no ESP-IDF dependency, so they unit-test on
//! the host with a mock bus.

pub mod aw9523;
pub mod bmi270;
pub mod axp2101;
pub mod ft6336;
pub mod py32;
pub mod scs;
pub mod si12t;

#[cfg(test)]
pub(crate) mod mock;

use embedded_hal::i2c::I2c;

/// Shared register helpers for 8-bit-register I2C devices.
pub(crate) fn write_reg<I: I2c>(i2c: &mut I, addr: u8, reg: u8, val: u8) -> Result<(), I::Error> {
    i2c.write(addr, &[reg, val])
}

pub(crate) fn read_reg<I: I2c>(i2c: &mut I, addr: u8, reg: u8) -> Result<u8, I::Error> {
    let mut b = [0u8];
    i2c.write_read(addr, &[reg], &mut b)?;
    Ok(b[0])
}
