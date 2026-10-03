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
    pub body: Option<Py32<Bus>>,
    pub lcd: Lcd,
    pub panel: Panel,
}

impl Board {
    pub fn init(i2c: I2C1<'static>, sda: Gpio12<'static>, scl: Gpio11<'static>) -> Result<Board> {
        // Long timeout: the PY32 body expander is an MCU and stretches the clock.
        let cfg = I2cConfig::new()
            .baudrate(Hertz(400_000))
            .sda_enable_pullup(true)
            .scl_enable_pullup(true)
            .timeout(Duration::from_millis(10).into());
        let driver = I2cDriver::new(i2c, sda, scl, &cfg)?;
        let bus: &'static Mutex<I2cDriver<'static>> = Box::leak(Box::new(Mutex::new(driver)));
        let dev = || MutexDevice::new(bus);

        let mut pmic = Axp2101::new(dev());
        pmic.init().map_err(|e| anyhow!("AXP2101: {e:?}"))?;
        info!("AXP2101 ok, battery {} %", pmic.battery_pct().unwrap_or(0));

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

        let mut head = Si12t::new(dev());
        let head = match head.init() {
            Ok(()) => Some(head),
            Err(e) => {
                warn!("Si12T head touch: {e:?}");
                None
            }
        };

        // The body expander boots slowly; give it 1.2 s like the BSP does.
        let mut body = Py32::new(dev());
        let started = Instant::now();
        let body = loop {
            FreeRtos::delay_ms(200);
            if let Ok(Some(v)) = body.version() {
                info!("PY32 body expander v{v}");
                break body.init(false).ok().map(|_| body);
            }
            if started.elapsed() > Duration::from_millis(1200) {
                warn!("PY32 body expander not found");
                break None;
            }
        };

        Ok(Board { pmic, touch, head, body, lcd, panel })
    }
}
