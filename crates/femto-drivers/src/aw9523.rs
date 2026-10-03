//! AW9523B IO expander (0x58): LCD and speaker-amp resets, camera/LCD rails.

use embedded_hal::delay::DelayNs;
use embedded_hal::i2c::I2c;

use crate::write_reg;

pub const ADDR: u8 = 0x58;

pub struct Aw9523<I> {
    i2c: I,
}

impl<I: I2c> Aw9523<I> {
    pub fn new(i2c: I) -> Self {
        Aw9523 { i2c }
    }

    pub fn release(self) -> I {
        self.i2c
    }

    /// Factory port setup.
    pub fn init(&mut self) -> Result<(), I::Error> {
        for (reg, val) in [(0x02, 0b0000_0111), (0x03, 0b1000_1111), (0x04, 0b0001_1000), (0x05, 0b0000_1100), (0x11, 0b0001_0000), (0x12, 0xFF), (0x13, 0xFF)] {
            write_reg(&mut self.i2c, ADDR, reg, val)?;
        }
        Ok(())
    }

    pub fn reset_lcd(&mut self, d: &mut impl DelayNs) -> Result<(), I::Error> {
        write_reg(&mut self.i2c, ADDR, 0x03, 0b1000_0001)?;
        d.delay_ms(20);
        write_reg(&mut self.i2c, ADDR, 0x03, 0b1000_0011)?;
        d.delay_ms(10);
        Ok(())
    }

    pub fn reset_speaker_amp(&mut self, d: &mut impl DelayNs) -> Result<(), I::Error> {
        write_reg(&mut self.i2c, ADDR, 0x02, 0b0000_0011)?;
        d.delay_ms(10);
        write_reg(&mut self.i2c, ADDR, 0x02, 0b0000_0111)?;
        d.delay_ms(50);
        Ok(())
    }
}
