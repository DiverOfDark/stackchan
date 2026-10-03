//! Femto firmware entry point.
//!
//! M0/M1 on hardware: board bring-up, the real engine + renderer on the LCD,
//! touch → Ledger, power key → factory-wipe confirm, head pat → Amused.
//! Wi-Fi, clock and live usage come from the `net` task.

mod board;
mod dns;
mod hub;
mod store;
mod tz;
mod vision;
mod voice;
mod web;
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

    // TCP/IP stack up before the web server and the Wi-Fi task race for it.
    sys::esp!(unsafe { sys::esp_netif_init() })?;
    let store = store::Store::open(nvs.clone())?;
    let (ui_tx, ui_rx) = std::sync::mpsc::channel();
    let (net_cmd_tx, net_cmd_rx) = std::sync::mpsc::channel();
    let hub = hub::Hub::new(store, ui_tx, net_cmd_tx);
    let mut cfg = hub.lock().unwrap().live.clone();
    let mut cfg_rev = 0;
    let mut engine = Engine::new();
    engine.apply_settings(&cfg);
    engine.set_screen(Screen::Boot);
    let (net_tx, net_rx) = std::sync::mpsc::channel();
    let net_cfg = net::NetConfig::load(&hub.lock().unwrap().store, &nvs);
    info!("Wi-Fi '{}', usage {}", net_cfg.ssid, if net_cfg.usage_url.is_empty() { "<unset>" } else { &net_cfg.usage_url });
    net::spawn(p.modem, nvs.clone(), net_cfg, hub.clone(), net_cmd_rx, net_tx)?;
    let (sight_tx, sight_rx) = std::sync::mpsc::channel();
    report_memory_tag("before vision");
    if cfg.camera {
        vision::spawn(1, sight_tx);
        std::thread::sleep(Duration::from_millis(1500));
    }
    report_memory_tag("after vision");
    let mut tracker = Tracker::default();
    let mut voice: Option<voice::Voice> = None;
    let mut last_pat: Option<Instant> = None;
    let _web = web::start(&hub)?;
    report_memory_tag("after web");
    let _mdns = esp_idf_svc::mdns::EspMdns::take().and_then(|mut m| {
        m.set_hostname("femto")?;
        m.add_service(Some("Femto"), "_http", "_tcp", 80, &[("path", "/")])?;
        Ok(m)
    });
    if let Err(e) = &_mdns {
        warn!("mDNS: {e}");
    }
    let mut ota_confirmed = false;
    // Screen the network wants once the boot animation has played.
    let mut after_boot: Option<Screen> = None;
    let mut wall_unix: Option<i64> = None;
    let mut booted = false;

    let mut renderer = Renderer::new();
    let mut canvas = Canvas::new();

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
    psram_stack_thread("lcd", 8192, move || {
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
    let boot_at = last;
    let mut last_frame = None;
    let mut touching = false;
    let mut patting = false;
    let mut wipe_deadline: Option<Instant> = None;
    let (mut render_us, mut push_us, mut frames) = (0u64, 0u64, 0u32);
    let mut fps = 0.0f32;
    let mut last_report = Instant::now();

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
                // Double pat = push-to-talk (PRD §5.4); a single pat amuses him.
                if last_pat.is_some_and(|t| t.elapsed() < Duration::from_millis(700)) {
                    if let Some(v) = &voice {
                        v.push_to_talk();
                    }
                    last_pat = None;
                } else {
                    engine.event(Event::HeadPat);
                    last_pat = Some(Instant::now());
                }
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

        // Settings changed from the web UI: apply live.
        {
            let h = hub.lock().unwrap();
            if h.rev != cfg_rev {
                cfg_rev = h.rev;
                let tz_changed = h.live.tz != cfg.tz;
                cfg = h.live.clone();
                drop(h);
                engine.apply_settings(&cfg);
                board.pmic.set_brightness(cfg.brightness.unwrap_or(60)).ok();
                if let Some(v) = &voice {
                    v.set_volume(cfg.volume);
                }
                if tz_changed {
                    if let Some(u) = engine.unix_now().or(wall_unix) {
                        engine.set_wall_clock(u, tz::offset(&cfg.tz, u));
                    }
                }
            }
        }
        while let Ok(cmd) = ui_rx.try_recv() {
            info!("ui: {cmd:?}");
            match cmd {
                hub::UiCmd::Trigger(t) => trigger(&mut engine, &cfg, t),
                hub::UiCmd::Reboot => {
                    std::thread::sleep(Duration::from_millis(500));
                    vision::stop();
                    unsafe { sys::esp_restart() };
                }
                hub::UiCmd::FactoryReset => factory_wipe(),
            }
        }
        if !ota_confirmed && start.duration_since(boot_at) > Duration::from_secs(60) {
            // Healthy for a minute: keep this image (PRD FR-25 rollback).
            ota_confirmed = true;
            match esp_idf_svc::ota::EspOta::new().and_then(|mut o| o.mark_running_slot_valid()) {
                Ok(()) => info!("firmware marked valid"),
                Err(e) => warn!("mark valid: {e}"),
            }
        }

        if let Some(v) = voice.as_mut() {
            v.sync(&mut engine);
        }

        // Network → engine.
        while let Ok(ev) = net_rx.try_recv() {
            info!("net: {ev:?}");
            match ev {
                net::NetEvent::Connecting { attempt, ssid } => after_boot = Some(Screen::Wifi { attempt, ssid }),
                net::NetEvent::Connected { .. } => {
                    after_boot = Some(Screen::Face);
                    report_memory_tag("wifi connected");
                    if voice.is_none() {
                        let url = hub.lock().unwrap().store.get(store::KEY_VOICE_URL).unwrap_or_default();
                        voice = voice::start(&url, cfg.volume);
                    }
                }
                net::NetEvent::Setup { ssid, key } => after_boot = Some(Screen::Setup { ap_ssid: ssid, ap_key: key, ip: "192.168.4.1".into() }),
                net::NetEvent::Clock { unix } => {
                    wall_unix = Some(unix);
                    engine.set_wall_clock(unix, tz::offset(&cfg.tz, unix));
                }
                net::NetEvent::Usage(u) => engine.set_usage(u),
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

        while let Ok(sight) = sight_rx.try_recv() {
            match sight {
                vision::Sight::Face { nx, ny, .. } => {
                    if tracker.lost_for() > Duration::from_secs(3) {
                        engine.event(Event::NewFace);
                    }
                    tracker.seen(nx, ny);
                    log::debug!("face nx {nx:+.2} ny {ny:+.2} → head yaw {:+.1} pitch {:+.1}", tracker.yaw, tracker.pitch);
                    engine.face_seen(nx, ny);
                }
                vision::Sight::Nobody => {
                    if tracker.lost_for() > Duration::from_millis(600) {
                        engine.face_lost();
                    }
                }
            }
        }
        if let Some(head) = &head {
            let (pan, tilt) = tracker.head(&engine, cfg.follow);
            let mut t = head.lock().unwrap();
            // Engine pan + = viewer's right = robot's left (yaw −).
            t.yaw = -pan;
            t.pitch = motion::PITCH_NEUTRAL + tilt;
            // Keep looking for faces even in Standby; rest only without a camera.
            t.may_rest = !cfg.camera && engine.resolve_emotion() == femto_core::Emotion::Sleepy;
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
        {
            let fr = last_frame.as_ref().unwrap();
            let mut h = hub.lock().unwrap();
            let s = &mut h.snap;
            s.mood = fr.em.label().to_string();
            s.screen = screen_name(&fr.screen).to_string();
            s.panel = format!("{:?}", board.panel);
            s.signed_in = fr.usage.signed_in;
            s.session_pct = fr.usage.session_pct;
            s.week_pct = fr.usage.week_pct;
            s.session_reset_min = fr.usage.session_reset_min;
            s.stale = fr.usage.stale;
            s.fps = fps;
            s.face = tracker.last_face();
            s.face_age_s = tracker.last_seen.map(|t| t.elapsed().as_secs_f32());
        }
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
            fps = frames as f32 * 1000.0 / last_report.elapsed().as_millis().max(1) as f32;
            last_report = Instant::now();
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

/// Wipe Wi-Fi, tokens, settings and password (namespaces `femto` and the
/// stock `wifi`), keep M5's servo calibration, and reboot into setup.
fn factory_wipe() -> ! {
    vision::stop();
    for ns in [c"femto", c"wifi"] {
        // SAFETY: NVS handle opened, erased, committed and closed here.
        unsafe {
            let mut h: sys::nvs_handle_t = 0;
            if sys::nvs_open(ns.as_ptr(), sys::nvs_open_mode_t_NVS_READWRITE, &mut h) == sys::ESP_OK {
                sys::nvs_erase_all(h);
                sys::nvs_commit(h);
                sys::nvs_close(h);
            }
        }
    }
    unsafe { sys::esp_restart() }
}

/// Closed-loop head tracking: the camera sits in the head, so a face's
/// offset is an error to integrate, not a target angle.
#[derive(Default)]
struct Tracker {
    yaw: f32,
    pitch: f32,
    last_seen: Option<Instant>,
    last_step: Option<Instant>,
    err: (f32, f32),
    started: Option<Instant>,
}

impl Tracker {
    const GAIN_DEG_S: f32 = 40.0;

    fn seen(&mut self, nx: f32, ny: f32) {
        self.last_seen = Some(Instant::now());
        self.err = (nx, ny);
    }

    fn epoch(&mut self) -> Instant {
        *self.started.get_or_insert_with(Instant::now)
    }

    fn last_face(&self) -> Option<(f32, f32)> {
        self.last_seen.map(|_| self.err)
    }

    fn lost_for(&self) -> Duration {
        self.last_seen.map_or(Duration::MAX, |t| t.elapsed())
    }

    /// (pan, tilt) in engine convention (pan + = viewer's right, tilt + = up).
    fn head(&mut self, engine: &Engine, follow: bool) -> (f32, f32) {
        let now = Instant::now();
        let dt = self.last_step.map_or(0.0, |t| (now - t).as_secs_f32()).min(0.2);
        self.last_step = Some(now);
        let em = engine.resolve_emotion();
        let tracking = follow && self.lost_for() < Duration::from_millis(1200) && !matches!(em, femto_core::Emotion::Sleepy | femto_core::Emotion::Thinking);
        if tracking {
            // Deadband so the head doesn't hunt around a centred face.
            let (ex, ey) = self.err;
            if ex.abs() > 0.12 {
                self.yaw += ex * Self::GAIN_DEG_S * dt;
            }
            if ey.abs() > 0.15 {
                self.pitch -= ey * Self::GAIN_DEG_S * 0.6 * dt;
            }
            self.yaw = self.yaw.clamp(-45.0, 45.0);
            self.pitch = self.pitch.clamp(-20.0, 30.0);
            (self.yaw, self.pitch)
        } else if !follow {
            self.yaw = 0.0;
            self.pitch = 0.0;
            engine.head_target()
        } else if self.lost_for() > Duration::from_secs(6) {
            // Nobody around: look about for a face (the camera only sees
            // where the head points). A new glance every ~4 s.
            const GLANCES: [(f32, f32); 6] = [(0.0, 6.0), (-25.0, 10.0), (20.0, 2.0), (0.0, 15.0), (25.0, 10.0), (-15.0, 0.0)];
            let slot = (now.duration_since(self.epoch()).as_secs() / 4) as usize % GLANCES.len();
            let (gy, gp) = GLANCES[slot];
            let k = (dt * 1.5).min(1.0);
            self.yaw += (gy - self.yaw) * k;
            self.pitch += (gp - self.pitch) * k;
            (self.yaw, self.pitch)
        } else {
            (self.yaw, self.pitch)
        }
    }
}

/// Spawn a thread whose stack lives in PSRAM (saves internal RAM). Only for
/// threads that never write flash: their stack is unreachable while the
/// flash cache is off.
pub fn psram_stack_thread<F: FnOnce() + Send + 'static>(name: &str, stack: usize, f: F) -> std::io::Result<std::thread::JoinHandle<()>> {
    psram_stack_thread_on(name, stack, None, f)
}

/// As [`psram_stack_thread`], pinned to `core` when given (the UI loop runs
/// on core 0; heavy workers go to core 1).
pub fn psram_stack_thread_on<F: FnOnce() + Send + 'static>(name: &str, stack: usize, core: Option<i32>, f: F) -> std::io::Result<std::thread::JoinHandle<()>> {
    // SAFETY: esp_pthread config is copied by value; restored right after spawn.
    unsafe {
        let mut cfg = sys::esp_pthread_get_default_config();
        cfg.stack_alloc_caps = sys::MALLOC_CAP_SPIRAM | sys::MALLOC_CAP_8BIT;
        cfg.stack_size = stack;
        if let Some(c) = core {
            cfg.pin_to_core = c;
        }
        sys::esp_pthread_set_cfg(&cfg);
    }
    let h = std::thread::Builder::new().name(name.into()).stack_size(stack).spawn(f);
    // SAFETY: as above.
    unsafe {
        let cfg = sys::esp_pthread_get_default_config();
        sys::esp_pthread_set_cfg(&cfg);
    }
    h
}

fn screen_name(s: &Screen) -> &'static str {
    match s {
        Screen::Boot => "boot",
        Screen::Wifi { .. } => "wifi",
        Screen::Setup { .. } => "setup",
        Screen::Face => "face",
        Screen::Ledger => "ledger",
        Screen::Listening => "listening",
        Screen::Thinking => "thinking",
        Screen::Speaking => "speaking",
        Screen::Wipe { .. } => "wipe",
    }
}

/// Test-panel triggers from the web UI.
fn trigger(engine: &mut Engine, cfg: &Settings, t: hub::Trigger) {
    use femto_core::{Emotion, VoiceState};
    if let Some(demo) = t.demo.as_deref() {
        match demo {
            "ask" => engine.run_demo(cfg),
            "power-on" => engine.run_power_on_demo(false, cfg),
            "first-run" => engine.run_power_on_demo(true, cfg),
            _ => {}
        }
    }
    if let Some(screen) = t.screen.as_deref() {
        engine.cancel_demo();
        match screen {
            "boot" => engine.set_screen(Screen::Boot),
            "wifi" => engine.set_screen(Screen::Wifi { attempt: 2, ssid: "HOME-WIFI".into() }),
            "setup" => engine.set_screen(Screen::Setup { ap_ssid: format!("{}-SETUP", cfg.name.to_uppercase()), ap_key: "7F3A-9C21".into(), ip: "192.168.4.1".into() }),
            "ledger" => engine.set_screen(Screen::Ledger),
            "listening" => engine.voice(VoiceState::Listening(format!("{}{}", cfg.name, femto_core::text::QUESTION_SUFFIX))),
            "thinking" => engine.voice(VoiceState::Thinking),
            "speaking" => engine.voice(VoiceState::Speaking(format!("Reset soon, {}. Try not to waste it.", cfg.honorific.as_str()))),
            _ => engine.set_screen(Screen::Face),
        }
    }
    if let Some(mood) = t.mood.as_deref() {
        let em = match mood {
            "neutral" => Some(Emotion::Neutral),
            "happy" => Some(Emotion::Happy),
            "excited" => Some(Emotion::Excited),
            "curious" => Some(Emotion::Curious),
            "surprised" => Some(Emotion::Surprised),
            "sleepy" => Some(Emotion::Sleepy),
            "worried" => Some(Emotion::Worried),
            _ => None,
        };
        engine.set_screen(Screen::Face);
        engine.set_override(em);
    }
}

fn report_memory_tag(tag: &str) {
    // SAFETY: plain FFI getters.
    let (free, largest) = unsafe { (sys::heap_caps_get_free_size(sys::MALLOC_CAP_INTERNAL), sys::heap_caps_get_largest_free_block(sys::MALLOC_CAP_INTERNAL)) };
    info!("internal heap [{tag}]: {} KB free, largest block {} KB", free / 1024, largest / 1024);
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
