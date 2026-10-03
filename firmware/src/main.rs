//! Femto firmware entry point.
//!
//! M0/M1 on hardware: board bring-up, the real engine + renderer on the LCD,
//! touch → Ledger, power key → factory-wipe confirm, head pat → Amused.
//! Wi-Fi, clock and live usage come from the `net` task.

mod board;
mod lcd;
mod motion;
mod net;

use std::time::{Duration, Instant};

use esp_idf_svc::hal::peripherals::Peripherals;
use esp_idf_svc::sys;
use femto_core::{Engine, Event, Screen, Settings};
use femto_render::{Canvas, Renderer};
use log::{info, warn};

/// The design animates on a 70 ms tick; rendering faster shows nothing new.
const FRAME: Duration = Duration::from_millis(femto_core::TICK_MS as u64);
const WIPE_WINDOW_S: u8 = 5;

fn main() -> anyhow::Result<()> {
    sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();
    info!("femto {} booting", env!("CARGO_PKG_VERSION"));

    let p = Peripherals::take()?;
    let nvs = esp_idf_svc::nvs::EspDefaultNvsPartition::take()?;
    let mut board = board::Board::init(p.i2c1, p.pins.gpio12, p.pins.gpio11)?;
    report_memory();

    let cfg = Settings::default();
    let mut engine = Engine::new();
    engine.apply_settings(&cfg);
    engine.set_screen(Screen::Boot);
    let (net_tx, net_rx) = std::sync::mpsc::channel();
    let net_cfg = net::NetConfig::load(&nvs);
    info!("Wi-Fi '{}', usage {}", net_cfg.ssid, if net_cfg.usage_url.is_empty() { "<unset>" } else { &net_cfg.usage_url });
    net::spawn(p.modem, nvs.clone(), net_cfg, net_tx)?;
    // Screen the network wants once the boot animation has played.
    let mut after_boot: Option<Screen> = None;
    let mut booted = false;

    let mut renderer = Renderer::new();
    let mut canvas = Canvas::new();
    // Rasterizer scratch in internal RAM (20 KB).
    canvas.set_scratch({
        let n = (femto_render::W + 2) * 16;
        // SAFETY: fresh internal allocation of n f32s, len 0; freed via free().
        let ptr = unsafe { sys::heap_caps_malloc(n * 4, sys::MALLOC_CAP_INTERNAL | sys::MALLOC_CAP_8BIT) } as *mut f32;
        assert!(!ptr.is_null(), "raster scratch");
        unsafe { Vec::from_raw_parts(ptr, 0, n) }
    });
    board.pmic.set_brightness(60).ok();
    info!("panel {:?}", board.panel);
    let head = match board.body.as_mut() {
        Some(body) => {
            // Dim accent glow (PRD §5.6).
            body.fill_leds(24, 2, 2).ok();
            body.set_servo_power(true).ok();
            std::thread::sleep(Duration::from_millis(200));
            match motion::start(p.uart1, p.pins.gpio6, p.pins.gpio7, nvs.clone()) {
                Ok(t) => Some(t),
                Err(e) => {
                    warn!("motion: {e}");
                    None
                }
            }
        }
        None => None,
    };

    // LCD scan-out runs on its own thread so it overlaps the next render.
    let (to_lcd, lcd_rx) = std::sync::mpsc::sync_channel::<Canvas>(1);
    let (back_tx, from_lcd) = std::sync::mpsc::sync_channel::<(Canvas, u32)>(1);
    let mut lcd = board.lcd;
    std::thread::Builder::new().name("lcd".into()).stack_size(8192).spawn(move || {
        for c in lcd_rx {
            let t = Instant::now();
            if let Err(e) = lcd.push(&c) {
                warn!("lcd push: {e}");
            }
            if back_tx.send((c, t.elapsed().as_micros() as u32)).is_err() {
                break;
            }
        }
    })?;
    let mut spare = Some(Canvas::new());

    let mut last = Instant::now();
    let mut last_frame = None;
    let mut touching = false;
    let mut patting = false;
    let mut wipe_deadline: Option<Instant> = None;
    let (mut render_us, mut push_us, mut frames) = (0u64, 0u64, 0u32);

    loop {
        let start = Instant::now();
        engine.advance((start - last).as_millis() as u32);
        last = start;

        // Screen tap: Face ⇄ Ledger, or cancel a pending wipe.
        let touch = board.touch.read().ok().flatten();
        if touch.is_some() && !touching {
            if wipe_deadline.take().is_some() {
                engine.set_screen(Screen::Face);
            } else {
                engine.tap();
            }
        }
        touching = touch.is_some();

        // Head pat.
        if let Some(head) = board.head.as_mut() {
            let pat = head.read().map(|z| z.iter().any(|&v| v > 0)).unwrap_or(false);
            if pat && !patting {
                engine.event(Event::HeadPat);
            }
            patting = pat;
        }

        // Power key: long press arms the wipe, a short press confirms it.
        if let Ok(k) = board.pmic.power_key() {
            if k.long {
                wipe_deadline = Some(Instant::now() + Duration::from_secs(WIPE_WINDOW_S as u64));
            } else if k.short && wipe_deadline.is_some() {
                warn!("factory wipe confirmed");
                factory_wipe();
            }
        }
        if let Some(deadline) = wipe_deadline {
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                wipe_deadline = None;
                engine.set_screen(Screen::Face);
            } else {
                engine.set_screen(Screen::Wipe { secs_left: left.as_secs() as u8 + 1 });
            }
        }

        // Network → engine.
        while let Ok(ev) = net_rx.try_recv() {
            info!("net: {ev:?}");
            match ev {
                net::NetEvent::Connecting { attempt, ssid } => after_boot = Some(Screen::Wifi { attempt, ssid }),
                net::NetEvent::Connected { .. } => after_boot = Some(Screen::Face),
                net::NetEvent::NoCredentials | net::NetEvent::Failed => {
                    after_boot = Some(Screen::Setup { ap_ssid: format!("{}-SETUP", cfg.name.to_uppercase()), ap_key: "----".into(), ip: "192.168.4.1".into() })
                }
                net::NetEvent::Clock { unix, utc_offset_s } => engine.set_wall_clock(unix, utc_offset_s),
                net::NetEvent::Usage(u) => engine.set_usage(u),
                net::NetEvent::UsageError(_) => {}
            }
        }
        if !booted && engine.frame().progress >= 1.0 {
            booted = true;
        }
        if booted {
            if let Some(s) = after_boot.take() {
                let to_face = s == Screen::Face && !matches!(engine.screen(), Screen::Face | Screen::Ledger);
                if !matches!(engine.screen(), Screen::Face | Screen::Ledger) || s != Screen::Face {
                    engine.set_screen(s);
                }
                if to_face {
                    engine.event(Event::BootDone);
                }
            }
        }

        if let Some(head) = &head {
            let (pan, tilt) = engine.head_target();
            let mut t = head.lock().unwrap();
            // Engine pan + = viewer's right = robot's left (yaw −).
            t.yaw = -pan;
            t.pitch = motion::PITCH_NEUTRAL + tilt;
            t.may_rest = engine.resolve_emotion() == femto_core::Emotion::Sleepy;
        }
        let frame = engine.frame();
        if last_frame.as_ref() == Some(&frame) {
            idle_sleep(start);
            continue;
        }
        let t0 = Instant::now();
        renderer.render(&mut canvas, &frame, &cfg);
        last_frame = Some(frame);
        render_us += t0.elapsed().as_micros() as u64;
        // Hand the frame to the LCD thread; continue on the other buffer.
        let next = match spare.take() {
            Some(c) => c,
            None => {
                let (c, us) = from_lcd.recv()?;
                push_us += us as u64;
                c
            }
        };
        let done = std::mem::replace(&mut canvas, next);
        to_lcd.send(done)?;
        frames += 1;
        if frames == 10 {
            let [bg, face, chrome, ov] = renderer.timings;
            info!("render {:.1} ms (bg {bg} face {face} chrome {chrome} overlay {ov} us), push {:.1} ms, mood {:?}", render_us as f32 / 10_000.0, push_us as f32 / 10_000.0, engine.resolve_emotion());
            let prof: Vec<u32> = femto_render::scene::PROF.iter().map(|a| a.swap(0, std::sync::atomic::Ordering::Relaxed) / 10).collect();
            info!("face stages us: {prof:?}");
            report_memory();
            (render_us, push_us, frames) = (0, 0, 0);
        }

        idle_sleep(start);
    }
}

/// Sleep out the rest of the frame (at least 1 ms so idle tasks run).
fn idle_sleep(start: Instant) {
    let rest = FRAME.checked_sub(start.elapsed()).unwrap_or_default();
    std::thread::sleep(rest.max(Duration::from_millis(1)));
}

fn factory_wipe() -> ! {
    // SAFETY: erases the default NVS partition, then restarts.
    unsafe {
        sys::nvs_flash_erase();
        sys::esp_restart()
    }
}

fn report_memory() {
    // SAFETY: plain FFI getters with no preconditions.
    let (internal, psram, psram_total) = unsafe {
        (
            sys::heap_caps_get_free_size(sys::MALLOC_CAP_INTERNAL),
            sys::heap_caps_get_free_size(sys::MALLOC_CAP_SPIRAM),
            sys::heap_caps_get_total_size(sys::MALLOC_CAP_SPIRAM),
        )
    };
    info!("heap: internal free {} KB, psram free {} / {} KB", internal / 1024, psram / 1024, psram_total / 1024);
}
