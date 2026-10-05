//! StackChan board bring-up (order from M5's factory firmware):
//! I2C → AXP2101 → AW9523 → FT6336 (panel id) → LCD → body expander.

use std::sync::Mutex;
use std::time::{Duration, Instant};

use anyhow::{anyhow, Result};
use embedded_hal_bus::i2c::MutexDevice;
use esp_idf_svc::hal::delay::FreeRtos;
use esp_idf_svc::hal::i2c::{I2cConfig, I2cDriver, I2C1};
use esp_idf_svc::hal::gpio::{Gpio11, Gpio12};
use esp_idf_svc::hal::units::Hertz;
use femto_drivers::aw9523::Aw9523;
use femto_drivers::axp2101::Axp2101;
use femto_drivers::bmi270::Bmi270;
use femto_drivers::ft6336::{Ft6336, Panel};
use femto_drivers::py32::Py32;
use femto_drivers::si12t::Si12t;
use log::{info, warn};

use crate::lcd::Lcd;

pub type Bus = MutexDevice<'static, I2cDriver<'static>>;

pub struct Board {
    pub pmic: Axp2101<Bus>,
    pub touch: Ft6336<Bus>,
    pub head: Option<Si12t<Bus>>,
    /// CoreS3 IMU (in the head): notices the robot being carried.
    pub imu: Option<Bmi270<Bus>>,
    /// Body expander, once it answered (see `body_probe`).
    pub body: Option<Py32<Bus>>,
    /// Not found at boot: keep probing (a soft reset doesn't reset the body,
    /// and the PY32 can take a while to answer again).
    pub body_probe: Option<Py32<Bus>>,
    pub lcd: Lcd,
    pub panel: Panel,
}

impl Board {
    pub fn init(i2c: I2C1<'static>, sda: Gpio12<'static>, scl: Gpio11<'static>) -> Result<Board> {
        bus_recovery(12, 11);
        // Longest clock-stretch timeout the S3 supports: the PY32 body expander is
        // an MCU, stretches the clock, and can stretch for a long time after a
        // reset caught it mid-transaction (M5's firmware waits up to 1 s).
        let cfg = I2cConfig::new()
            // 100 kHz: what M5 and every other PY32 driver use (400 kHz wedged it).
            .baudrate(Hertz(100_000))
            .sda_enable_pullup(true)
            .scl_enable_pullup(true)
            .timeout(Duration::from_millis(100).into()); // hardware max (2^22 XTAL cycles)
        let driver = I2cDriver::new(i2c, sda, scl, &cfg)?;
        let bus: &'static Mutex<I2cDriver<'static>> = Box::leak(Box::new(Mutex::new(driver)));
        let dev = || MutexDevice::new(bus);

        let mut pmic = Axp2101::new(dev());
        pmic.init().map_err(|e| anyhow!("AXP2101: {e:?}"))?;
        info!("AXP2101 ok, battery {} %", pmic.battery_pct().unwrap_or(0));
        // The PMIC latches why it last powered off: the only witness when
        // the ESP32 itself loses power (no crash dump, USB just vanishes).
        match pmic.power_history() {
            Ok(h) => warn!(
                "power history: last off by {:?} (0x{:02x}), on by {:?} (0x{:02x}), VBUS {}",
                h.off_reasons(), h.off_src, h.on_reasons(), h.on_src, if h.vbus_good() { "good" } else { "absent" }
            ),
            Err(e) => warn!("power history: {e:?}"),
        }
        info!("battery charge current {} mA", pmic.charge_current_ma().unwrap_or(0));

        let mut aw = Aw9523::new(dev());
        aw.init().map_err(|e| anyhow!("AW9523: {e:?}"))?;
        FreeRtos::delay_ms(50);

        let mut touch = Ft6336::new(dev());
        let panel = (0..5)
            .find_map(|_| match touch.panel() {
                Ok(Some(p)) => Some(p),
                _ => {
                    FreeRtos::delay_ms(20);
                    None
                }
            })
            .unwrap_or_else(|| {
                warn!("FT6336 version unreadable; assuming ILI9342C");
                Panel::Ili9342c
            });
        info!("LCD panel: {panel:?}");

        aw.reset_lcd(&mut FreeRtos).map_err(|e| anyhow!("LCD reset: {e:?}"))?;
        let lcd = Lcd::new(panel)?;

        {
            use embedded_hal::i2c::I2c;
            let mut d = dev();
            let mut found = Vec::new();
            for a in 0x08u8..0x78 {
                let mut b = [0u8];
                if d.write_read(a, &[0], &mut b).is_ok() {
                    found.push(format!("{a:02x}"));
                }
            }
            info!("i2c devices: {}", found.join(" "));
        }
        let mut head = Si12t::new(dev());
        let head = match head.init() {
            Ok(()) => Some(head),
            Err(e) => {
                warn!("Si12T head touch: {e:?}");
                None
            }
        };

        let imu = match Bmi270::init(dev(), FreeRtos::delay_ms) {
            Ok(imu) => {
                info!("BMI270 IMU up");
                Some(imu)
            }
            Err((_, e)) => {
                warn!("BMI270 IMU: {e:?}");
                None
            }
        };

        // The body expander boots slowly; give it 1.2 s like the BSP does,
        // then the UI loop keeps probing in the background.
        let mut body = Py32::new(dev());
        let started = Instant::now();
        let (body, body_probe) = loop {
            FreeRtos::delay_ms(200);
            if let Ok(Some(v)) = body.version() {
                info!("PY32 body expander v{v}");
                break match body.init(false) {
                    Ok(()) => (Some(body), None),
                    Err(e) => {
                        warn!("PY32 init: {e:?}");
                        (None, Some(body))
                    }
                };
            }
            if started.elapsed() > Duration::from_millis(1200) {
                warn!("PY32 body expander not answering yet; will keep probing");
                break (None, Some(body));
            }
        };

        Ok(Board { pmic, touch, head, imu, body, body_probe, lcd, panel })
    }
}

/// I2C bus recovery before the driver starts: 9 SCL pulses with SDA released,
/// then a STOP. A soft reset of the ESP32 doesn't reset the body board, and
/// the PY32 (an MCU acting as an I2C slave) stays wedged if the reset caught
/// it mid-transaction; this clocks it out of that state.
fn bus_recovery(sda: i32, scl: i32) {
    use esp_idf_svc::sys::*;
    // SAFETY: raw GPIO setup on the I2C pins before the I2C driver owns them.
    unsafe {
        let od = gpio_mode_t_GPIO_MODE_INPUT_OUTPUT_OD;
        for pin in [sda, scl] {
            gpio_reset_pin(pin);
            gpio_set_direction(pin, od);
            gpio_set_pull_mode(pin, gpio_pull_mode_t_GPIO_PULLUP_ONLY);
            gpio_set_level(pin, 1);
        }
        esp_rom_delay_us(10);
        let stuck = gpio_get_level(sda) == 0;
        for _ in 0..9 {
            gpio_set_level(scl, 0);
            esp_rom_delay_us(10);
            gpio_set_level(scl, 1);
            esp_rom_delay_us(10);
        }
        // STOP: SDA low → high while SCL is high.
        gpio_set_level(sda, 0);
        esp_rom_delay_us(10);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(10);
        gpio_set_level(sda, 1);
        esp_rom_delay_us(10);
        if stuck {
            warn!("I2C SDA was held low at boot; bus recovered: {}", gpio_get_level(sda) == 1);
        }
    }
}
