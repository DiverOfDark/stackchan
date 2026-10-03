//! Femto firmware entry point.
//!
//! M0/M1 on hardware: board bring-up, the real engine + renderer on the LCD,
//! touch → Ledger, power key → factory-wipe confirm, head pat → Amused.
//! Usage is mocked until the network milestone (M2).

mod board;
mod lcd;

use std::time::{Duration, Instant};

use esp_idf_svc::hal::peripherals::Peripherals;
use esp_idf_svc::sys;
use femto_core::{Engine, Event, Screen, Settings, Usage};
use femto_render::{Canvas, Renderer};
use log::{info, warn};

const FRAME: Duration = Duration::from_millis(40);
const WIPE_WINDOW_S: u8 = 5;

fn main() -> anyhow::Result<()> {
    sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();
    info!("femto {} booting", env!("CARGO_PKG_VERSION"));

    let p = Peripherals::take()?;
    let mut board = board::Board::init(p.i2c1, p.pins.gpio12, p.pins.gpio11)?;
    report_memory();

    let cfg = Settings::default();
    let mut engine = Engine::new();
    engine.apply_settings(&cfg);
    // No clock yet (SNTP comes with Wi-Fi); anchor at a fixed instant.
    engine.set_wall_clock(1_791_400_000, 2 * 3600);
    engine.set_usage(Usage {
        signed_in: true,
        ok: true,
        fetched_at: Some(1_791_400_000),
        session_pct: 38,
        session_resets_at: Some(1_791_400_000 + 134 * 60),
        week_pct: 61,
        week_resets_at: Some(1_791_400_000 + 5 * 86_400),
        ..Default::default()
    });
    engine.run_power_on_demo(false, &cfg);

    let mut renderer = Renderer::new();
    let mut canvas = Canvas::new();
    board.pmic.set_brightness(60).ok();
    info!("panel {:?}", board.panel);
    if let Some(body) = board.body.as_mut() {
        // Dim accent glow (PRD §5.6).
        body.fill_leds(24, 2, 2).ok();
    }

    let mut last = Instant::now();
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

        let t0 = Instant::now();
        renderer.render(&mut canvas, &engine.frame(), &cfg);
        let t1 = Instant::now();
        if let Err(e) = board.lcd.push(canvas.pixels()) {
            warn!("lcd push: {e}");
        }
        render_us += (t1 - t0).as_micros() as u64;
        push_us += t1.elapsed().as_micros() as u64;
        frames += 1;
        if frames == 100 {
            info!("render {:.1} ms, push {:.1} ms, mood {:?}", render_us as f32 / 100_000.0, push_us as f32 / 100_000.0, engine.resolve_emotion());
            report_memory();
            (render_us, push_us, frames) = (0, 0, 0);
        }

        if let Some(rest) = FRAME.checked_sub(start.elapsed()) {
            std::thread::sleep(rest);
        } else {
            // Yield so the idle task can feed the watchdog.
            std::thread::sleep(Duration::from_millis(1));
        }
    }
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
