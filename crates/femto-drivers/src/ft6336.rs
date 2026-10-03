//! FT6336U capacitive touch (0x38). Also tells the LCD revision apart:
//! firmware id 0x12 + vendor 0x11 ships with the ILI9342E panel.

use embedded_hal::i2c::I2c;

pub const ADDR: u8 = 0x38;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Panel {
    Ili9342c,
    Ili9342e,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Touch {
    pub x: u16,
    pub y: u16,
}

pub struct Ft6336<I> {
    i2c: I,
}

impl<I: I2c> Ft6336<I> {
    pub fn new(i2c: I) -> Self {
        Ft6336 { i2c }
    }

    pub fn release(self) -> I {
        self.i2c
    }

    /// `None` if the version can't be read (factory firmware then assumes C).
    pub fn panel(&mut self) -> Result<Option<Panel>, I::Error> {
        self.i2c.write(ADDR, &[0x00, 0x00])?;
        let mut info = [0u8; 6];
        self.i2c.write_read(ADDR, &[0xA3], &mut info)?;
        if info[0] == 0 || info[5] == 0 {
            return Ok(None);
        }
        Ok(Some(if info[3] == 0x12 && info[5] == 0x11 { Panel::Ili9342e } else { Panel::Ili9342c }))
    }

    pub fn read(&mut self) -> Result<Option<Touch>, I::Error> {
        let mut b = [0u8; 5];
        self.i2c.write_read(ADDR, &[0x02], &mut b)?;
        if b[0] & 0x0F == 0 {
            return Ok(None);
        }
        Ok(Some(Touch { x: ((b[1] as u16 & 0x0F) << 8) | b[2] as u16, y: ((b[3] as u16 & 0x0F) << 8) | b[4] as u16 }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mock::MockI2c;

    #[test]
    fn detects_e_panel() {
        let mut m = MockI2c::default();
        for (i, v) in [0x64, 0, 0, 0x12, 0, 0x11].into_iter().enumerate() {
            m.regs.insert((ADDR, 0xA3 + i as u8), v);
        }
        assert_eq!(Ft6336::new(m).panel().unwrap(), Some(Panel::Ili9342e));
    }

    #[test]
    fn reads_point() {
        let mut m = MockI2c::default();
        for (i, v) in [1, 0x01, 0x20, 0x00, 0x50].into_iter().enumerate() {
            m.regs.insert((ADDR, 0x02 + i as u8), v);
        }
        assert_eq!(Ft6336::new(m).read().unwrap(), Some(Touch { x: 0x120, y: 0x50 }));
    }
}
