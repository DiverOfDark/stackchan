//! PY32L020 body IO expander (0x6F): servo power (pin 0), 12 RGB LEDs (pin 13).

use embedded_hal::i2c::I2c;

use crate::{read_reg, write_reg};

pub const ADDR: u8 = 0x6F;

const REG_VERSION: u8 = 0x02;
const REG_GPIO_M_L: u8 = 0x03;
const REG_GPIO_O_L: u8 = 0x05;
const REG_GPIO_PU_L: u8 = 0x09;
const REG_GPIO_PD_L: u8 = 0x0B;
const REG_GPIO_DRV_L: u8 = 0x13;
const REG_LED_CFG: u8 = 0x24;
const REG_LED_RAM: u8 = 0x30;

pub const PIN_SERVO_POWER: u8 = 0;
pub const PIN_LEDS: u8 = 13;
pub const LED_COUNT: u8 = 12;

pub struct Py32<I> {
    i2c: I,
}

impl<I: I2c> Py32<I> {
    pub fn new(i2c: I) -> Self {
        Py32 { i2c }
    }

    pub fn release(self) -> I {
        self.i2c
    }

    /// `Some(version)` once the expander has booted (it boots slowly).
    pub fn version(&mut self) -> Result<Option<u8>, I::Error> {
        let v = read_reg(&mut self.i2c, ADDR, REG_VERSION)?;
        Ok((v != 0 && v != 0xFF).then_some(v))
    }

    fn write_bit(&mut self, reg_l: u8, pin: u8, on: bool) -> Result<(), I::Error> {
        let (reg, bit) = if pin < 8 { (reg_l, pin) } else { (reg_l + 1, pin - 8) };
        let v = read_reg(&mut self.i2c, ADDR, reg)?;
        let n = if on { v | (1 << bit) } else { v & !(1 << bit) };
        write_reg(&mut self.i2c, ADDR, reg, n)
    }

    /// Output, pull-up, push-pull.
    fn output(&mut self, pin: u8) -> Result<(), I::Error> {
        self.write_bit(REG_GPIO_M_L, pin, true)?;
        self.write_bit(REG_GPIO_PD_L, pin, false)?;
        self.write_bit(REG_GPIO_PU_L, pin, true)?;
        self.write_bit(REG_GPIO_DRV_L, pin, false)
    }

    /// Factory setup with the LED strip dark. Servo power stays as asked:
    /// the motion task turns it on once it is ready to hold position.
    pub fn init(&mut self, servo_power: bool) -> Result<(), I::Error> {
        self.output(PIN_SERVO_POWER)?;
        self.set_servo_power(servo_power)?;
        self.output(PIN_LEDS)?;
        write_reg(&mut self.i2c, ADDR, REG_LED_CFG, LED_COUNT & 0x3F)?;
        self.fill_leds(0, 0, 0)
    }

    pub fn set_servo_power(&mut self, on: bool) -> Result<(), I::Error> {
        self.write_bit(REG_GPIO_O_L, PIN_SERVO_POWER, on)
    }

    /// Set all LEDs (RGB888 → RGB565 little-endian in LED RAM) and latch.
    pub fn set_leds(&mut self, colors: &[(u8, u8, u8)]) -> Result<(), I::Error> {
        let mut buf = [0u8; 1 + 2 * LED_COUNT as usize];
        buf[0] = REG_LED_RAM;
        for (i, &(r, g, b)) in colors.iter().take(LED_COUNT as usize).enumerate() {
            let v = ((r as u16 & 0xF8) << 8) | ((g as u16 & 0xFC) << 3) | (b as u16 >> 3);
            buf[1 + i * 2] = v as u8;
            buf[2 + i * 2] = (v >> 8) as u8;
        }
        self.i2c.write(ADDR, &buf[..1 + 2 * colors.len().min(LED_COUNT as usize)])?;
        let cfg = read_reg(&mut self.i2c, ADDR, REG_LED_CFG)?;
        write_reg(&mut self.i2c, ADDR, REG_LED_CFG, cfg | (1 << 6))
    }

    pub fn fill_leds(&mut self, r: u8, g: u8, b: u8) -> Result<(), I::Error> {
        self.set_leds(&[(r, g, b); LED_COUNT as usize])
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mock::MockI2c;

    #[test]
    fn servo_power_bit() {
        let mut p = Py32::new(MockI2c::default());
        p.init(true).unwrap();
        let m = p.release();
        assert_eq!(m.regs[&(ADDR, REG_GPIO_O_L)] & 1, 1);
        assert_eq!(m.regs[&(ADDR, REG_GPIO_M_L + 1)] & (1 << 5), 1 << 5, "pin 13 output");
        assert_eq!(m.regs[&(ADDR, REG_LED_CFG)] & 0x3F, 12);
    }

    #[test]
    fn led_encoding() {
        let mut p = Py32::new(MockI2c::default());
        p.set_leds(&[(255, 0, 0)]).unwrap();
        let m = p.release();
        assert_eq!(m.regs[&(ADDR, REG_LED_RAM)], 0x00);
        assert_eq!(m.regs[&(ADDR, REG_LED_RAM + 1)], 0xF8);
    }
}
