//! BMI270 6-axis IMU in the CoreS3 (0x69). The CoreS3 is the StackChan's
//! head, so it moves with the servos.
//!
//! The chip needs Bosch's 8 KB feature config uploaded after reset
//! (`bmi270_config.bin`, from Bosch's BMI270 SensorAPI, BSD-3-Clause, as
//! shipped in M5Unified).

use embedded_hal::i2c::I2c;

use crate::{read_reg, write_reg};

pub const ADDR: u8 = 0x69;
const CHIP_ID: u8 = 0x24;

const REG_CHIP_ID: u8 = 0x00;
const REG_ACC_X: u8 = 0x0C;
const REG_INTERNAL_STATUS: u8 = 0x21;
const REG_ACC_CONF: u8 = 0x40;
const REG_ACC_RANGE: u8 = 0x41;
const REG_GYR_CONF: u8 = 0x42;
const REG_GYR_RANGE: u8 = 0x43;
const REG_INIT_CTRL: u8 = 0x59;
const REG_INIT_ADDR_0: u8 = 0x5B;
const REG_INIT_DATA: u8 = 0x5E;
const REG_PWR_CONF: u8 = 0x7C;
const REG_PWR_CTRL: u8 = 0x7D;
const REG_CMD: u8 = 0x7E;
const CMD_SOFT_RESET: u8 = 0xB6;

static CONFIG: &[u8; 8192] = include_bytes!("bmi270_config.bin");
/// Config upload chunk (one I2C write each; the address auto-increments).
const CHUNK: usize = 64;

/// ±4 g full scale.
const ACC_LSB_PER_G: f32 = 8192.0;
/// ±2000 °/s full scale.
const GYR_LSB_PER_DPS: f32 = 16.4;

/// One reading: acceleration in g, rotation rate in °/s (chip axes).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Sample {
    pub acc: [f32; 3],
    pub gyr: [f32; 3],
}

impl Sample {
    pub fn acc_g(&self) -> f32 {
        self.acc.iter().map(|a| a * a).sum::<f32>().sqrt()
    }

    pub fn gyr_dps(&self) -> f32 {
        self.gyr.iter().map(|g| g * g).sum::<f32>().sqrt()
    }
}

#[derive(Debug)]
pub enum Error<E> {
    Bus(E),
    /// Wrong or missing chip id.
    NotFound(u8),
    /// The feature engine never reported "init OK".
    InitFailed(u8),
}

impl<E> From<E> for Error<E> {
    fn from(e: E) -> Self {
        Error::Bus(e)
    }
}

pub struct Bmi270<I> {
    i2c: I,
}

impl<I: I2c> Bmi270<I> {
    /// Reset, upload the config and start accel (100 Hz) + gyro (200 Hz).
    /// `delay_ms` sleeps; init takes ~30 ms.
    pub fn init(mut i2c: I, mut delay_ms: impl FnMut(u32)) -> Result<Self, (I, Error<I::Error>)> {
        match Self::setup(&mut i2c, &mut delay_ms) {
            Ok(()) => Ok(Bmi270 { i2c }),
            Err(e) => Err((i2c, e)),
        }
    }

    fn setup(i2c: &mut I, delay_ms: &mut impl FnMut(u32)) -> Result<(), Error<I::Error>> {
        let id = read_reg(i2c, ADDR, REG_CHIP_ID)?;
        if id != CHIP_ID {
            return Err(Error::NotFound(id));
        }
        write_reg(i2c, ADDR, REG_CMD, CMD_SOFT_RESET)?;
        delay_ms(2);
        // Advanced power save off, so the config can be written.
        write_reg(i2c, ADDR, REG_PWR_CONF, 0x00)?;
        delay_ms(1);
        write_reg(i2c, ADDR, REG_INIT_CTRL, 0x00)?;
        let mut buf = [0u8; CHUNK + 1];
        for (n, chunk) in CONFIG.chunks(CHUNK).enumerate() {
            // INIT_ADDR counts 16-bit words: low 4 bits, then the rest.
            let word = n * CHUNK / 2;
            i2c.write(ADDR, &[REG_INIT_ADDR_0, (word & 0x0F) as u8, (word >> 4) as u8])?;
            buf[0] = REG_INIT_DATA;
            buf[1..=chunk.len()].copy_from_slice(chunk);
            i2c.write(ADDR, &buf[..=chunk.len()])?;
        }
        write_reg(i2c, ADDR, REG_INIT_CTRL, 0x01)?;
        let mut status = 0;
        for _ in 0..20 {
            delay_ms(5);
            status = read_reg(i2c, ADDR, REG_INTERNAL_STATUS)? & 0x0F;
            if status == 0x01 {
                break;
            }
        }
        if status != 0x01 {
            return Err(Error::InitFailed(status));
        }
        // acc: ODR 100 Hz, normal averaging, performance mode; ±4 g.
        write_reg(i2c, ADDR, REG_ACC_CONF, 0xA8)?;
        write_reg(i2c, ADDR, REG_ACC_RANGE, 0x01)?;
        // gyr: ODR 200 Hz, normal filter, performance mode; ±2000 °/s.
        write_reg(i2c, ADDR, REG_GYR_CONF, 0xA9)?;
        write_reg(i2c, ADDR, REG_GYR_RANGE, 0x00)?;
        write_reg(i2c, ADDR, REG_PWR_CTRL, 0x06)?;
        Ok(())
    }

    pub fn read(&mut self) -> Result<Sample, I::Error> {
        let mut b = [0u8; 12];
        self.i2c.write_read(ADDR, &[REG_ACC_X], &mut b)?;
        let v = |i: usize| i16::from_le_bytes([b[i * 2], b[i * 2 + 1]]) as f32;
        Ok(Sample {
            acc: [v(0) / ACC_LSB_PER_G, v(1) / ACC_LSB_PER_G, v(2) / ACC_LSB_PER_G],
            gyr: [v(3) / GYR_LSB_PER_DPS, v(4) / GYR_LSB_PER_DPS, v(5) / GYR_LSB_PER_DPS],
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mock::MockI2c;

    #[test]
    fn rejects_wrong_chip() {
        let mut m = MockI2c::default();
        m.regs.insert((ADDR, REG_CHIP_ID), 0x00);
        assert!(matches!(Bmi270::init(m, |_| {}), Err((_, Error::NotFound(0)))));
    }

    #[test]
    fn uploads_config_and_reads() {
        let mut m = MockI2c::default();
        m.regs.insert((ADDR, REG_CHIP_ID), CHIP_ID);
        m.regs.insert((ADDR, REG_INTERNAL_STATUS), 0x01);
        let Ok(mut imu) = Bmi270::init(m, |_| {}) else { panic!("init") };
        let data: usize = imu.i2c.writes.iter().filter(|(_, w)| w[0] == REG_INIT_DATA).map(|(_, w)| w.len() - 1).sum();
        assert_eq!(data, 8192);
        // Last chunk's word address: (8192 - 64) / 2 = 4064 → 0x0, 0xFE.
        assert!(imu.i2c.writes.iter().any(|(_, w)| w == &[REG_INIT_ADDR_0, 0x00, 0xFE]));
        // 1 g on z, 90 °/s about x.
        imu.i2c.regs.insert((ADDR, REG_ACC_X + 4), 0x00);
        imu.i2c.regs.insert((ADDR, REG_ACC_X + 5), 0x20);
        let gx = (90.0 * GYR_LSB_PER_DPS) as i16;
        imu.i2c.regs.insert((ADDR, REG_ACC_X + 6), gx.to_le_bytes()[0]);
        imu.i2c.regs.insert((ADDR, REG_ACC_X + 7), gx.to_le_bytes()[1]);
        let s = imu.read().unwrap();
        assert!((s.acc_g() - 1.0).abs() < 1e-3);
        assert!((s.gyr_dps() - 90.0).abs() < 0.1);
    }
}
