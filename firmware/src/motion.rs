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

/// Critically damped spring, softer than M5's BSP motion (stiffness 170):
/// slower moves are quieter. Tunable live, as is the speed cap.
pub const STIFFNESS: f32 = 80.0;
/// Top head speed, degrees per second.
pub const MAX_SPEED: f32 = 60.0;
const DT: f32 = 0.02;
/// Neutral pitch: slightly up, so following can look down a little.
/// Neutral pitch: the camera sits in the head, so look up at a seated face.
pub const PITCH_NEUTRAL: f32 = 25.0;
pub const YAW_LIMIT: f32 = 60.0;
pub const PITCH_MIN: f32 = 0.0;
pub const PITCH_MAX: f32 = 60.0;
/// Torque off after resting this long (no buzz, less power).
const REST_TORQUE_OFF: Duration = Duration::from_secs(10);
/// Each 20 ms setpoint is sent as a move lasting this long. Overlapping
/// moves let the servo blend them into one motion instead of a 50 Hz
/// start-stop staircase (audible as buzz).
pub const MOVE_MS: u16 = 60;
/// Servo feedback is checked this often (UART time is shared with moves).
const FEEDBACK_EVERY: u32 = 5;
/// Head this far from where it's driven, twice in a row = a hand holds it.
const GRAB_DEG: f32 = 12.0;
/// A held head that stops moving for this long has been let go.
const RELEASE_AFTER: Duration = Duration::from_secs(2);

/// What the main loop wants: degrees, yaw + = robot's right, pitch + = up.
#[derive(Clone, Copy, Debug, Default)]
pub struct Target {
    pub yaw: f32,
    pub pitch: f32,
    /// Allowed to rest (Standby): torque may switch off once settled.
    pub may_rest: bool,
}

/// Shared between the UI loop, the web UI and the motion task.
#[derive(Debug)]
pub struct Motion {
    pub target: Target,
    /// Web UI jog: absolute pose held until the instant.
    pub manual: Option<(f32, f32, Instant)>,
    /// Web UI nod test until the instant.
    pub nod_until: Option<Instant>,
    /// Torque allowed at all (web UI switch).
    pub torque_allowed: bool,
    /// Current pose (degrees) and calibration (raw zero positions).
    pub pos: (f32, f32),
    pub zero: (u16, u16),
    /// Set by the web UI: re-zero both axes at the current pose.
    pub rezero: bool,
    /// Last time the head was moving (the camera sees its own motion).
    pub moved_at: Option<Instant>,
    /// Set by the UI loop from the IMU: the robot is being carried or
    /// bumped, so hold the pose until then.
    pub freeze_until: Option<Instant>,
    /// A hand is holding the head: torque is off so it can be posed, and
    /// back on once it's let go.
    pub grabbed: bool,
    /// Duration of each setpoint move (see [`MOVE_MS`]); tunable live.
    pub move_ms: u16,
    /// Spring stiffness and speed cap (see [`STIFFNESS`], [`MAX_SPEED`]).
    pub stiffness: f32,
    pub max_speed: f32,
    /// Pitch may lose torque at rest too (it carries the head and may sag);
    /// yaw always does.
    pub rest_pitch: bool,
}

impl Motion {
    /// Head still long enough that frame differences mean the scene moved.
    pub fn still_for(&self, d: Duration) -> bool {
        !self.grabbed && self.moved_at.is_none_or(|t| t.elapsed() > d)
    }

    /// Being handled: the head isn't following anything right now.
    pub fn handled(&self) -> bool {
        self.grabbed || self.freeze_until.is_some_and(|t| Instant::now() < t)
    }
}

pub type MotionRef = Arc<Mutex<Motion>>;

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
    fn step(&mut self, target: f32, stiffness: f32, max_speed: f32) {
        let acc = stiffness * (target - self.pos) - 2.0 * stiffness.sqrt() * self.vel;
        self.vel = (self.vel + acc * DT).clamp(-max_speed, max_speed);
        self.pos += self.vel * DT;
    }

    fn settled(&self, target: f32) -> bool {
        (target - self.pos).abs() < 0.3 && self.vel.abs() < 0.5
    }

    fn raw(&self) -> u16 {
        scs::raw_from_decidegrees(self.zero, (self.pos * 10.0) as i32)
    }
}

/// Start the motion task. Servo power must already be on. `still`: skip
/// the boot stretch.
pub fn start(uart: UART1<'static>, tx: Gpio6<'static>, rx: Gpio7<'static>, nvs: EspDefaultNvsPartition, still: bool) -> Result<MotionRef> {
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

    let target = Arc::new(Mutex::new(Motion {
        target: Target { yaw: 0.0, pitch: PITCH_NEUTRAL, may_rest: false },
        manual: None,
        nod_until: None,
        torque_allowed: true,
        pos: (yaw.pos, pitch.pos),
        zero: (yaw.zero, pitch.zero),
        rezero: false,
        moved_at: None,
        freeze_until: None,
        grabbed: false,
        move_ms: MOVE_MS,
        stiffness: STIFFNESS,
        max_speed: MAX_SPEED,
        rest_pitch: false,
    }));
    let shared = target.clone();
    crate::psram_stack_thread("motion", 6144, move || {
        // Torque on per axis: (yaw, pitch).
        let mut torque = (false, false);
        let mut resting_since: Option<Instant> = None;
        let mut errors = 0u32;
        let started = Instant::now();
        let mut tick = 0u32;
        // Grab detection: strikes above GRAB_DEG; while grabbed, the last
        // read pose and since when it hasn't changed.
        let mut strikes = 0u8;
        let mut grabbed = false;
        let mut held_pose = (0.0f32, 0.0f32);
        let mut held_still_since = Instant::now();
        // Last goal written per axis (None = unknown, e.g. after limp).
        let mut sent_raw: (Option<u16>, Option<u16>) = (None, None);
        loop {
            tick = tick.wrapping_add(1);
            let (mut t, allowed, frozen, move_ms, stiffness, max_speed, rest_pitch) = {
                let mut m = shared.lock().unwrap();
                if std::mem::take(&mut m.rezero) {
                    // Current pose becomes the new centre (raw zero), so the
                    // head doesn't move; the web handler persists it.
                    yaw.zero = yaw.raw();
                    pitch.zero = pitch.raw();
                    yaw.pos = 0.0;
                    pitch.pos = 0.0;
                    yaw.vel = 0.0;
                    pitch.vel = 0.0;
                    m.zero = (yaw.zero, pitch.zero);
                    info!("servo zero now yaw {} pitch {}", yaw.zero, pitch.zero);
                }
                let mut t = m.target;
                let now = Instant::now();
                if let Some((y, p, until)) = m.manual {
                    if now < until {
                        t = Target { yaw: y, pitch: p, may_rest: false };
                    } else {
                        m.manual = None;
                    }
                }
                if let Some(until) = m.nod_until {
                    if now < until {
                        let ph = (until - now).as_secs_f32() * std::f32::consts::TAU * 1.2;
                        t.pitch = PITCH_NEUTRAL + 12.0 * ph.sin();
                        t.yaw = 0.0;
                        t.may_rest = false;
                    } else {
                        m.nod_until = None;
                    }
                }
                m.pos = (yaw.pos, pitch.pos);
                if yaw.vel.abs() > 0.5 || pitch.vel.abs() > 0.5 || grabbed {
                    m.moved_at = Some(now);
                }
                m.grabbed = grabbed;
                let frozen = m.freeze_until.is_some_and(|u| now < u);
                (t, m.torque_allowed, frozen, m.move_ms, m.stiffness, m.max_speed, m.rest_pitch)
            };
            // Servo feedback: is a hand forcing the head, or has it let go?
            if torque.0 || torque.1 || grabbed {
                if tick % FEEDBACK_EVERY == 0 {
                    let read = |bus: &mut ScsBus<Uart>, a: &Axis| bus.read_pos(a.id).ok().map(|raw| scs::decidegrees_from_raw(a.zero, raw) as f32 / 10.0);
                    if let (Some(ay), Some(ap)) = (read(&mut bus, &yaw), read(&mut bus, &pitch)) {
                        if grabbed {
                            // Limp: the spring starts from wherever the hand leaves it.
                            if (ay - held_pose.0).abs() > 1.0 || (ap - held_pose.1).abs() > 1.0 {
                                held_still_since = Instant::now();
                            }
                            held_pose = (ay, ap);
                            yaw.pos = ay;
                            pitch.pos = ap;
                            yaw.vel = 0.0;
                            pitch.vel = 0.0;
                            if held_still_since.elapsed() > RELEASE_AFTER {
                                info!("head let go at yaw {ay:.1}° pitch {ap:.1}°");
                                grabbed = false;
                                strikes = 0;
                            }
                        } else if (torque.0 && (ay - yaw.pos).abs() > GRAB_DEG) || (torque.1 && (ap - pitch.pos).abs() > GRAB_DEG) {
                            // Only an axis under torque can be forced; a
                            // resting one may just have sagged.
                            strikes += 1;
                            if strikes >= 2 {
                                info!("head grabbed (driven {:.1}°/{:.1}°, at {ay:.1}°/{ap:.1}°): going limp", yaw.pos, pitch.pos);
                                grabbed = true;
                                held_pose = (ay, ap);
                                held_still_since = Instant::now();
                            }
                        } else {
                            strikes = 0;
                        }
                    }
                }
            }
            if frozen || grabbed {
                // Hold the pose: no spring, no boot stretch.
                t = Target { yaw: yaw.pos, pitch: pitch.pos, may_rest: false };
                yaw.vel = 0.0;
                pitch.vel = 0.0;
            }
            // Boot stretch: glance left, right, then hand over to the engine.
            match started.elapsed().as_millis() {
                _ if frozen || grabbed || still => {}
                0..700 => t = Target { yaw: -20.0, pitch: PITCH_NEUTRAL + 8.0, may_rest: false },
                700..1400 => t = Target { yaw: 20.0, pitch: PITCH_NEUTRAL + 8.0, may_rest: false },
                _ => {}
            }
            let ty = t.yaw.clamp(-YAW_LIMIT, YAW_LIMIT);
            let tp = t.pitch.clamp(PITCH_MIN, PITCH_MAX);
            yaw.step(ty, stiffness, max_speed);
            pitch.step(tp, stiffness, max_speed);
            let settled = yaw.settled(ty) && pitch.settled(tp);
            resting_since = match (settled && t.may_rest, resting_since) {
                (true, None) => Some(Instant::now()),
                (true, s) => s,
                (false, _) => None,
            };
            let limp = !allowed || grabbed;
            let idle = resting_since.is_some_and(|s| s.elapsed() > REST_TORQUE_OFF);
            let want = (!(limp || idle), !(limp || (idle && rest_pitch)));
            for (axis, on, want, sent) in [(&mut yaw, &mut torque.0, want.0, &mut sent_raw.0), (&mut pitch, &mut torque.1, want.1, &mut sent_raw.1)] {
                if *on == want {
                    continue;
                }
                if want && allowed && !grabbed {
                    // Resting, the axis may have sagged or been nudged:
                    // the spring starts from where it really is.
                    if let Ok(raw) = bus.read_pos(axis.id) {
                        axis.pos = scs::decidegrees_from_raw(axis.zero, raw) as f32 / 10.0;
                        axis.vel = 0.0;
                    }
                }
                // The goal register may be stale (limp, re-zero): aim at
                // the current pose before torque comes back.
                if want {
                    bus.write_pos(axis.id, axis.raw(), 0, 0).ok();
                }
                *sent = None;
                if let Err(e) = bus.torque(axis.id, want) {
                    errors += 1;
                    if errors % 100 == 1 {
                        warn!("servo {} torque: {e:?}", axis.id);
                    }
                }
                *on = want;
            }
            if !allowed {
                // Limp: follow the target in software so re-enabling is smooth.
                yaw.pos = ty;
                pitch.pos = tp;
            }
            if !settled {
                for (axis, sent, on) in [(&yaw, &mut sent_raw.0, torque.0), (&pitch, &mut sent_raw.1, torque.1)] {
                    // Same goal as last time: nothing new to tell the servo.
                    let raw = axis.raw();
                    if !on || *sent == Some(raw) {
                        continue;
                    }
                    *sent = Some(raw);
                    if let Err(e) = bus.write_pos(axis.id, raw, move_ms, 0) {
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
