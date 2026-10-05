//! AXP2101 PMIC (0x34): rails, backlight (DLDO1), power key.

use embedded_hal::i2c::I2c;

use crate::{read_reg, write_reg};

pub const ADDR: u8 = 0x34;

const REG_STATUS1: u8 = 0x00;
const REG_STATUS2: u8 = 0x01;
const REG_PWRON_SRC: u8 = 0x20;
const REG_PWROFF_SRC: u8 = 0x21;
const REG_DC_UVP_OVP_OFF: u8 = 0x23;
const REG_VBAT_H: u8 = 0x34;
const REG_VBUS_H: u8 = 0x38;
const REG_VSYS_H: u8 = 0x3A;
const REG_DC_PWM_CTRL: u8 = 0x81;
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

/// Why the PMIC last powered off (REG 0x21; latched until the next one).
const PWROFF_REASONS: [&str; 8] = [
    "power key held to off level",
    "software power-off",
    "power key low past threshold",
    "VSYS under-voltage",
    "VBUS over-voltage",
    "DCDC under-voltage",
    "DCDC over-voltage",
    "die over-temperature",
];
/// What powered it on (REG 0x20).
/// (XPowersLib bit order: 0 POWERON low, 1 IRQ low, 2 VBUS insert, 3
/// charging, 4 battery insert, 5 EN mode.)
const PWRON_REASONS: [&str; 6] = ["power key", "IRQ pin", "VBUS inserted", "battery charging", "battery inserted", "EN pin"];

/// Snapshot of the PMIC's own record of the last power cycle.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PowerHistory {
    pub on_src: u8,
    pub off_src: u8,
    pub status1: u8,
}

impl PowerHistory {
    pub fn on_reasons(&self) -> Vec<&'static str> {
        PWRON_REASONS.iter().enumerate().filter(|(i, _)| self.on_src & (1 << i) != 0).map(|(_, r)| *r).collect()
    }

    pub fn off_reasons(&self) -> Vec<&'static str> {
        PWROFF_REASONS.iter().enumerate().filter(|(i, _)| self.off_src & (1 << i) != 0).map(|(_, r)| *r).collect()
    }

    /// VBUS (USB 5 V) present and good.
    pub fn vbus_good(&self) -> bool {
        self.status1 & (1 << 5) != 0
    }
}

/// Battery and supply readings (ADC + charger status).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PowerNow {
    pub battery_pct: u8,
    pub vbat_mv: u16,
    pub vbus_mv: u16,
    pub vsys_mv: u16,
    pub vbus_good: bool,
    /// Battery current: charging (+1), discharging (-1) or idle (0).
    pub direction: i8,
    /// The charger is cutting its USB draw because VBUS sags: the supply
    /// (port, charger or cable) can't deliver what the robot uses.
    pub vindpm: bool,
}

impl PowerNow {
    /// On USB but draining the battery: it will run flat and power off.
    pub fn starved(&self) -> bool {
        self.vindpm || (self.vbus_good && self.direction < 0)
    }
}

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

    pub fn power_now(&mut self) -> Result<PowerNow, I::Error> {
        let s1 = read_reg(&mut self.i2c, ADDR, REG_STATUS1)?;
        let s2 = read_reg(&mut self.i2c, ADDR, REG_STATUS2)?;
        let mut adc = |h: u8| -> Result<u16, I::Error> {
            let hi = read_reg(&mut self.i2c, ADDR, h)?;
            let lo = read_reg(&mut self.i2c, ADDR, h + 1)?;
            Ok((((hi & 0x3F) as u16) << 8) | lo as u16)
        };
        let (vbat_mv, vbus_mv, vsys_mv) = (adc(REG_VBAT_H)?, adc(REG_VBUS_H)?, adc(REG_VSYS_H)?);
        Ok(PowerNow {
            battery_pct: read_reg(&mut self.i2c, ADDR, REG_BAT_PCT)?,
            vbat_mv,
            vbus_mv,
            vsys_mv,
            vbus_good: s1 & (1 << 5) != 0,
            direction: match (s2 >> 5) & 0b11 {
                0b01 => 1,
                0b10 => -1,
                _ => 0,
            },
            vindpm: s2 & (1 << 3) != 0,
        })
    }

    /// Constant-current charge setting (REG 62H, from efuse unless set).
    pub fn charge_current_ma(&mut self) -> Result<u16, I::Error> {
        let n = (read_reg(&mut self.i2c, ADDR, REG_ICC_CHG)? & 0x1F) as u16;
        Ok(if n <= 8 { 25 * n } else { 200 + 100 * (n - 8) })
    }

    /// The PMIC's latched power-on / power-off sources (read once at boot).
    pub fn power_history(&mut self) -> Result<PowerHistory, I::Error> {
        Ok(PowerHistory {
            on_src: read_reg(&mut self.i2c, ADDR, REG_PWRON_SRC)?,
            off_src: read_reg(&mut self.i2c, ADDR, REG_PWROFF_SRC)?,
            status1: read_reg(&mut self.i2c, ADDR, REG_STATUS1)?,
        })
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
        // A DCDC dipping 15% below target (servos starting under a weak USB
        // supply) powered the whole robot off until the button was pressed.
        // Don't: keep OVP (bit 5) and VSYS UVLO (2.6 V); a real sag on the
        // 3.3 V rail trips the ESP32's brown-out reset instead, which reboots
        // by itself. Longest UVP debounce (240 us) as well.
        let uvp = read_reg(&mut self.i2c, ADDR, REG_DC_UVP_OVP_OFF)?;
        write_reg(&mut self.i2c, ADDR, REG_DC_UVP_OVP_OFF, uvp & !0b1_1111)?;
        let pwm = read_reg(&mut self.i2c, ADDR, REG_DC_PWM_CTRL)?;
        write_reg(&mut self.i2c, ADDR, REG_DC_PWM_CTRL, pwm | 0b11)?;
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

    #[test]
    fn init_keeps_dcdc_ovp_but_not_uvp_power_off() {
        let mut m = MockI2c::default();
        m.regs.insert((ADDR, REG_DC_UVP_OVP_OFF), 0b0011_1111); // POR default
        let mut p = Axp2101::new(m);
        p.init().unwrap();
        let m = p.release();
        assert_eq!(m.regs[&(ADDR, REG_DC_UVP_OVP_OFF)], 0b0010_0000);
        assert_eq!(m.regs[&(ADDR, REG_DC_PWM_CTRL)] & 0b11, 0b11);
    }

    #[test]
    fn reads_power_now() {
        let mut m = MockI2c::default();
        m.regs.insert((ADDR, REG_STATUS1), 1 << 5);
        m.regs.insert((ADDR, REG_STATUS2), (0b10 << 5) | (1 << 3)); // discharging, VINDPM
        m.regs.insert((ADDR, REG_VBUS_H), 0x13);
        m.regs.insert((ADDR, REG_VBUS_H + 1), 0x88); // 0x1388 = 5000 mV
        m.regs.insert((ADDR, REG_BAT_PCT), 37);
        let p = Axp2101::new(m).power_now().unwrap();
        assert_eq!((p.vbus_mv, p.battery_pct, p.direction), (5000, 37, -1));
        assert!(p.vbus_good && p.vindpm && p.starved());
    }

    #[test]
    fn decodes_power_history() {
        let h = PowerHistory { on_src: 0b100, off_src: 0b10_0000, status1: 1 << 5 };
        assert_eq!(h.on_reasons(), vec!["VBUS inserted"]);
        assert_eq!(h.off_reasons(), vec!["DCDC under-voltage"]);
        assert!(h.vbus_good());
    }
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
