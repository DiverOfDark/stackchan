//! AXP2101 PMIC (0x34): rails, backlight (DLDO1), power key.

use embedded_hal::i2c::I2c;

use crate::{read_reg, write_reg};

pub const ADDR: u8 = 0x34;

const REG_STATUS2: u8 = 0x01;
const REG_PWROFF_EN: u8 = 0x10;
const REG_IRQ_LEVEL: u8 = 0x27;
const REG_ADC_EN: u8 = 0x30;
const REG_IRQ_EN1: u8 = 0x41;
const REG_IRQ_STATUS1: u8 = 0x49;
const REG_ICC_CHG: u8 = 0x62;
const REG_CHG_LED: u8 = 0x69;
const REG_LDO_EN: u8 = 0x90;
const REG_ALDO3_V: u8 = 0x94;
const REG_ALDO4_V: u8 = 0x95;
const REG_BLDO2_V: u8 = 0x97;
const REG_DLDO1_V: u8 = 0x99;
const REG_BAT_PCT: u8 = 0xA4;

/// Power-key IRQ bits in IRQ status 1 (0x49).
const PKEY_SHORT: u8 = 1 << 3;
const PKEY_LONG: u8 = 1 << 2;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct PowerKey {
    pub short: bool,
    pub long: bool,
}

pub struct Axp2101<I> {
    i2c: I,
}

impl<I: I2c> Axp2101<I> {
    pub fn new(i2c: I) -> Self {
        Axp2101 { i2c }
    }

    pub fn release(self) -> I {
        self.i2c
    }

    /// Same rail setup as the factory firmware, plus power-key timing for
    /// PRD D4: long-press IRQ at 2.5 s, hard power-off at 10 s.
    pub fn init(&mut self) -> Result<(), I::Error> {
        let en = read_reg(&mut self.i2c, ADDR, REG_LDO_EN)?;
        write_reg(&mut self.i2c, ADDR, REG_LDO_EN, en | 0b1011_0100)?;
        write_reg(&mut self.i2c, ADDR, REG_BLDO2_V, 0b11110 - 2)?;
        write_reg(&mut self.i2c, ADDR, REG_CHG_LED, 0b0011_0101)?;
        write_reg(&mut self.i2c, ADDR, REG_ADC_EN, 0b11_1111)?;
        write_reg(&mut self.i2c, ADDR, REG_LDO_EN, 0xBF)?;
        write_reg(&mut self.i2c, ADDR, REG_ALDO3_V, 33 - 5)?;
        write_reg(&mut self.i2c, ADDR, REG_ALDO4_V, 33 - 5)?;
        // IRQ level / off time: [5:4] IRQ long-press time 0b11 = 2.5 s,
        // [1:0] power-off time 0b11 = 10 s.
        write_reg(&mut self.i2c, ADDR, REG_IRQ_LEVEL, 0b0011_0011)?;
        // Power key long-press must not shut down by itself (bit 1 of 0x10 off).
        let off = read_reg(&mut self.i2c, ADDR, REG_PWROFF_EN)?;
        write_reg(&mut self.i2c, ADDR, REG_PWROFF_EN, off & !0b10)?;
        let irq = read_reg(&mut self.i2c, ADDR, REG_IRQ_EN1)?;
        write_reg(&mut self.i2c, ADDR, REG_IRQ_EN1, irq | PKEY_SHORT | PKEY_LONG)?;
        self.set_charge_current_700ma()?;
        self.set_brightness(0)
    }

    fn set_charge_current_700ma(&mut self) -> Result<(), I::Error> {
        let v = read_reg(&mut self.i2c, ADDR, REG_ICC_CHG)?;
        write_reg(&mut self.i2c, ADDR, REG_ICC_CHG, (v & 0xE0) | 14)
    }

    /// 0 = backlight off, 1–100 maps to DLDO1 20..28 (factory mapping).
    pub fn set_brightness(&mut self, pct: u8) -> Result<(), I::Error> {
        let en = read_reg(&mut self.i2c, ADDR, REG_LDO_EN)?;
        if pct == 0 {
            return write_reg(&mut self.i2c, ADDR, REG_LDO_EN, en & 0x7F);
        }
        let v = 20 + (pct.min(100) as u16 * 8 / 100) as u8;
        write_reg(&mut self.i2c, ADDR, REG_DLDO1_V, v)?;
        if en & 0x80 == 0 {
            write_reg(&mut self.i2c, ADDR, REG_LDO_EN, en | 0x80)?;
        }
        Ok(())
    }

    /// Read and clear power-key events.
    pub fn power_key(&mut self) -> Result<PowerKey, I::Error> {
        let s = read_reg(&mut self.i2c, ADDR, REG_IRQ_STATUS1)?;
        let k = PowerKey { short: s & PKEY_SHORT != 0, long: s & PKEY_LONG != 0 };
        if k.short || k.long {
            write_reg(&mut self.i2c, ADDR, REG_IRQ_STATUS1, s & (PKEY_SHORT | PKEY_LONG))?;
        }
        Ok(k)
    }

    pub fn battery_pct(&mut self) -> Result<u8, I::Error> {
        read_reg(&mut self.i2c, ADDR, REG_BAT_PCT)
    }

    pub fn external_power(&mut self) -> Result<bool, I::Error> {
        let s = read_reg(&mut self.i2c, ADDR, REG_STATUS2)?;
        let dir = (s & 0b0110_0000) >> 5;
        Ok(dir != 2 || (s & 0b111) == 0b100)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mock::MockI2c;

    #[test]
    fn brightness_mapping() {
        let mut p = Axp2101::new(MockI2c::default());
        p.set_brightness(100).unwrap();
        let i2c = p.release();
        assert_eq!(i2c.regs[&(ADDR, REG_DLDO1_V)], 28);
        assert_eq!(i2c.regs[&(ADDR, REG_LDO_EN)] & 0x80, 0x80);
    }

    #[test]
    fn power_key_clears() {
        let mut m = MockI2c::default();
        m.regs.insert((ADDR, REG_IRQ_STATUS1), PKEY_LONG);
        let mut p = Axp2101::new(m);
        assert_eq!(p.power_key().unwrap(), PowerKey { short: false, long: true });
    }
}
