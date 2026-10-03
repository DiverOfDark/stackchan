//! Si12T 3-zone capacitive touch on top of the head (0x68).

use embedded_hal::i2c::I2c;

use crate::{read_reg, write_reg};

pub const ADDR: u8 = 0x68;

const REG_SENS1: u8 = 0x02;
const REG_CTRL1: u8 = 0x08;
const REG_CTRL2: u8 = 0x09;
const REG_REF_RST1: u8 = 0x0A;
const REG_OUTPUT1: u8 = 0x10;

/// Touch intensity per zone, front to back: 0 none, 1 low, 2 mid, 3 high.
pub type Zones = [u8; 3];

pub struct Si12t<I> {
    i2c: I,
}

impl<I: I2c> Si12t<I> {
    pub fn new(i2c: I) -> Self {
        Si12t { i2c }
    }

    pub fn release(self) -> I {
        self.i2c
    }

    /// Factory setup: all channels on, auto mode, sensitivity Low/3 (0x33).
    pub fn init(&mut self) -> Result<(), I::Error> {
        for reg in REG_REF_RST1..=0x0F {
            write_reg(&mut self.i2c, ADDR, reg, 0x00)?;
        }
        write_reg(&mut self.i2c, ADDR, REG_CTRL2, 0x0F)?;
        write_reg(&mut self.i2c, ADDR, REG_CTRL2, 0x07)?;
        // MS 0 | FTC 01 | ILC 00 | RTC 010
        write_reg(&mut self.i2c, ADDR, REG_CTRL1, 0b0010_0010)?;
        for reg in REG_SENS1..REG_SENS1 + 5 {
            write_reg(&mut self.i2c, ADDR, reg, 0x33)?;
        }
        Ok(())
    }

    pub fn read(&mut self) -> Result<Zones, I::Error> {
        let r = read_reg(&mut self.i2c, ADDR, REG_OUTPUT1)?;
        let ch = |j: u8| (r >> j) & 0x03;
        // Channel order is reversed relative to the head (BSP: intensities[2 - i]).
        Ok([ch(4), ch(2), ch(0)])
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mock::MockI2c;

    #[test]
    fn parses_zones() {
        let mut m = MockI2c::default();
        m.regs.insert((ADDR, REG_OUTPUT1), 0b00_10_00_01);
        // ch0 (bits 1:0) = 1, ch2 (bits 5:4) = 2 → zones reversed: [2, 0, 1].
        assert_eq!(Si12t::new(m).read().unwrap(), [2, 0, 1]);
    }
}
