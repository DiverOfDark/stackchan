//! Head motion: SCS0009 yaw/pitch servos on UART1 (TX 6, RX 7, 1 Mbaud).
//! A 50 Hz task springs toward the engine's head target (PRD §5.5).

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::Result;
use esp_idf_svc::hal::delay::TickType;
use esp_idf_svc::hal::gpio::{Gpio6, Gpio7};
use esp_idf_svc::hal::uart::{config::Config, UartDriver, UART1};
use esp_idf_svc::hal::units::Hertz;
use esp_idf_svc::nvs::{EspDefaultNvsPartition, EspNvs, NvsDefault};
use femto_drivers::scs::{self, ScsBus, ID_PITCH, ID_YAW};
use log::{info, warn};

/// Spring from M5's BSP motion (stiffness 170, damping 26, 50 Hz).
const STIFFNESS: f32 = 170.0;
const DAMPING: f32 = 26.0;
const DT: f32 = 0.02;
/// Neutral pitch: slightly up, so following can look down a little.
pub const PITCH_NEUTRAL: f32 = 12.0;
const YAW_LIMIT: f32 = 60.0;
const PITCH_MIN: f32 = 0.0;
const PITCH_MAX: f32 = 45.0;
/// Torque off after resting this long (no buzz, less power).
const REST_TORQUE_OFF: Duration = Duration::from_secs(10);

/// What the main loop wants: degrees, yaw + = robot's right, pitch + = up.
#[derive(Clone, Copy, Debug, Default)]
pub struct Target {
    pub yaw: f32,
    pub pitch: f32,
    /// Allowed to rest (Standby): torque may switch off once settled.
    pub may_rest: bool,
}

struct Uart(UartDriver<'static>);

impl scs::Bus for Uart {
    type Error = esp_idf_svc::sys::EspError;

    fn write_all(&mut self, mut bytes: &[u8]) -> Result<(), Self::Error> {
        while !bytes.is_empty() {
            let n = self.0.write(bytes)?;
            bytes = &bytes[n..];
        }
        self.0.wait_tx_done(TickType::from(Duration::from_millis(5)).ticks())
    }

    fn read_exact(&mut self, buf: &mut [u8], timeout_ms: u32) -> Result<(), Self::Error> {
        let deadline = Instant::now() + Duration::from_millis(timeout_ms as u64);
        let mut n = 0;
        while n < buf.len() {
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return Err(esp_idf_svc::sys::EspError::from_infallible::<{ esp_idf_svc::sys::ESP_ERR_TIMEOUT }>());
            }
            n += self.0.read(&mut buf[n..], TickType::from(left).ticks())?;
        }
        Ok(())
    }

    fn clear_rx(&mut self) {
        self.0.clear_rx().ok();
    }
}

struct Axis {
    id: u8,
    zero: u16,
    pos: f32,
    vel: f32,
}

impl Axis {
    fn step(&mut self, target: f32) {
        let acc = STIFFNESS * (target - self.pos) - DAMPING * self.vel;
        self.vel += acc * DT;
        self.pos += self.vel * DT;
    }

    fn settled(&self, target: f32) -> bool {
        (target - self.pos).abs() < 0.3 && self.vel.abs() < 0.5
    }

    fn raw(&self) -> u16 {
        scs::raw_from_decidegrees(self.zero, (self.pos * 10.0) as i32)
    }
}

/// Start the motion task. Servo power must already be on.
pub fn start(uart: UART1<'static>, tx: Gpio6<'static>, rx: Gpio7<'static>, nvs: EspDefaultNvsPartition) -> Result<Arc<Mutex<Target>>> {
    let driver = UartDriver::new(uart, tx, rx, Option::<esp_idf_svc::hal::gpio::AnyIOPin>::None, Option::<esp_idf_svc::hal::gpio::AnyIOPin>::None, &Config::new().baudrate(Hertz(scs::BAUD)))?;
    let mut bus = ScsBus::new(Uart(driver));

    // Calibration written by M5's firmware ("servo" namespace) or defaults.
    let store: Option<EspNvs<NvsDefault>> = EspNvs::new(nvs, "servo", false).ok();
    let zero = |key: &str, default: u16| {
        store
            .as_ref()
            .and_then(|s| s.get_i32(key).ok().flatten())
            .filter(|v| (scs::RAW_MIN as i32..=scs::RAW_MAX as i32).contains(v))
            .map_or(default, |v| v as u16)
    };
    let mut yaw = Axis { id: ID_YAW, zero: zero("zero_pos_1", 460), pos: 0.0, vel: 0.0 };
    let mut pitch = Axis { id: ID_PITCH, zero: zero("zero_pos_2", 620), pos: PITCH_NEUTRAL, vel: 0.0 };
    info!("servo zero: yaw {} pitch {}", yaw.zero, pitch.zero);

    // Start from where the head actually is, so power-up doesn't jerk it.
    for axis in [&mut yaw, &mut pitch] {
        match bus.read_pos(axis.id) {
            Ok(raw) => axis.pos = scs::decidegrees_from_raw(axis.zero, raw) as f32 / 10.0,
            Err(e) => warn!("servo {} read: {e:?}", axis.id),
        }
    }
    info!("head at yaw {:.1}° pitch {:.1}°", yaw.pos, pitch.pos);

    let target = Arc::new(Mutex::new(Target { yaw: 0.0, pitch: PITCH_NEUTRAL, may_rest: false }));
    let shared = target.clone();
    crate::psram_stack_thread("motion", 6144, move || {
        let mut torque = false;
        let mut resting_since: Option<Instant> = None;
        let mut errors = 0u32;
        let started = Instant::now();
        loop {
            let mut t = *shared.lock().unwrap();
            // Boot stretch: glance left, right, then hand over to the engine.
            match started.elapsed().as_millis() {
                0..700 => t = Target { yaw: -20.0, pitch: PITCH_NEUTRAL + 8.0, may_rest: false },
                700..1400 => t = Target { yaw: 20.0, pitch: PITCH_NEUTRAL + 8.0, may_rest: false },
                _ => {}
            }
            let ty = t.yaw.clamp(-YAW_LIMIT, YAW_LIMIT);
            let tp = t.pitch.clamp(PITCH_MIN, PITCH_MAX);
            yaw.step(ty);
            pitch.step(tp);
            let settled = yaw.settled(ty) && pitch.settled(tp);
            resting_since = match (settled && t.may_rest, resting_since) {
                (true, None) => Some(Instant::now()),
                (true, s) => s,
                (false, _) => None,
            };
            let rest = resting_since.is_some_and(|s| s.elapsed() > REST_TORQUE_OFF);
            if rest == torque {
                for axis in [&yaw, &pitch] {
                    if let Err(e) = bus.torque(axis.id, !rest) {
                        errors += 1;
                        if errors % 100 == 1 {
                            warn!("servo {} torque: {e:?}", axis.id);
                        }
                    }
                }
                torque = !rest;
            }
            if torque && !settled {
                for axis in [&yaw, &pitch] {
                    if let Err(e) = bus.write_pos(axis.id, axis.raw(), 20, 0) {
                        errors += 1;
                        if errors % 100 == 1 {
                            warn!("servo {} move: {e:?}", axis.id);
                        }
                    }
                }
            }
            std::thread::sleep(Duration::from_millis(20));
        }
    })?;
    Ok(target)
}
