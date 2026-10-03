//! Femto on the desktop. The mouse stands in for the camera's face tracking.
//!
//! Keys:
//!   click        tap screen (Face ⇄ Ledger)
//!   0 / 1–7      auto mood / force Contempt, Satisfied, Amused, Scanning, Alarmed, Standby, Rationing
//!   D            demo: "how much Claude do I have left?"
//!   B / N        demo: power on / first run (no Wi-Fi)
//!   Up / Down    session usage ±5 %      PgUp / PgDn  weekly usage ±5 %
//!   P            head pat                K            factory-wipe confirm screen
//!   E / A / C / X  cycle eyewear / accent, toggle corp / fx
//!   F            toggle follow           L            cycle LED mode
//!   Esc          quit

use std::time::{Duration, Instant};

use femto_core::settings::{Accent, Eyewear};
use femto_core::{Emotion, Engine, Event, Screen, Settings, Usage};
use femto_render::{Canvas, Renderer, H, W};
use minifb::{Key, KeyRepeat, MouseButton, MouseMode, Window, WindowOptions};

fn main() {
    // Screen plus a 16 px strip showing the 12 body LEDs (left 0–5, right 6–11).
    const LED_ROW: usize = 16;
    let mut window = Window::new(
        "Femto simulator",
        W,
        H + LED_ROW,
        WindowOptions { scale: minifb::Scale::X4, ..Default::default() },
    )
    .expect("open window");
    window.set_target_fps(30);

    let mut cfg = Settings::default();
    let mut engine = Engine::new();
    engine.apply_settings(&cfg);
    let start_unix = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs() as i64;
    engine.set_wall_clock(start_unix, 2 * 3600);
    let mut usage = Usage {
        signed_in: true,
        ok: true,
        fetched_at: Some(start_unix),
        session_pct: 38,
        session_resets_at: Some(start_unix + 134 * 60),
        week_pct: 61,
        week_resets_at: Some(start_unix + 5 * 86_400),
        ..Default::default()
    };
    engine.set_usage(usage.clone());
    engine.run_power_on_demo(false, &cfg);

    let mut renderer = Renderer::new();
    let mut canvas = Canvas::new();
    let mut buf = vec![0u32; W * (H + LED_ROW)];
    let started = Instant::now();
    let mut last = Instant::now();
    let mut was_down = false;
    let mut render_time = Duration::ZERO;
    let mut frames = 0u32;

    while window.is_open() && !window.is_key_down(Key::Escape) {
        let now = Instant::now();
        engine.advance((now - last).as_millis() as u32);
        last = now;

        match window.get_mouse_pos(MouseMode::Discard) {
            Some((mx, my)) => engine.face_seen(mx / W as f32 * 2.0 - 1.0, (my - H as f32 * 0.42) / (H as f32 / 2.0)),
            None => engine.face_lost(),
        }
        let down = window.get_mouse_down(MouseButton::Left);
        if down && !was_down {
            engine.tap();
        }
        was_down = down;

        let mut usage_changed = false;
        for key in window.get_keys_pressed(KeyRepeat::Yes) {
            match key {
                Key::Key0 => engine.set_override(None),
                Key::Key1 | Key::Key2 | Key::Key3 | Key::Key4 | Key::Key5 | Key::Key6 | Key::Key7 => {
                    let i = key as usize - Key::Key1 as usize;
                    engine.set_screen(Screen::Face);
                    engine.set_override(Some(Emotion::MANUAL[i]));
                }
                Key::D => engine.run_demo(&cfg),
                Key::B => engine.run_power_on_demo(false, &cfg),
                Key::N => engine.run_power_on_demo(true, &cfg),
                Key::Up | Key::Down => {
                    let d: i16 = if key == Key::Up { 5 } else { -5 };
                    usage.session_pct = (usage.session_pct as i16 + d).clamp(0, 100) as u8;
                    usage_changed = true;
                }
                Key::PageUp | Key::PageDown => {
                    let d: i16 = if key == Key::PageUp { 5 } else { -5 };
                    usage.week_pct = (usage.week_pct as i16 + d).clamp(0, 100) as u8;
                    usage_changed = true;
                }
                Key::P => engine.event(Event::HeadPat),
                Key::K => engine.set_screen(Screen::Wipe { secs_left: 5 }),
                Key::E => {
                    cfg.eyewear = match cfg.eyewear {
                        Eyewear::ExecGlasses => Eyewear::ArHalfLens,
                        Eyewear::ArHalfLens => Eyewear::Reticle,
                        Eyewear::Reticle => Eyewear::None,
                        Eyewear::None => Eyewear::ExecGlasses,
                    }
                }
                Key::A => {
                    cfg.accent = match cfg.accent {
                        Accent::SignalRed => Accent::ToxicGreen,
                        Accent::ToxicGreen => Accent::IceBlue,
                        Accent::IceBlue => Accent::SignalRed,
                    }
                }
                Key::C => cfg.corp = !cfg.corp,
                Key::X => cfg.fx = !cfg.fx,
                Key::L => {
                    use femto_core::settings::LedMode;
                    cfg.led_mode = match cfg.led_mode {
                        LedMode::Usage => LedMode::Mood,
                        LedMode::Mood => LedMode::Off,
                        LedMode::Off => LedMode::Usage,
                    }
                }
                Key::F => {
                    cfg.follow = !cfg.follow;
                    engine.apply_settings(&cfg);
                }
                _ => {}
            }
        }
        if usage_changed {
            engine.set_usage(usage.clone());
        }

        let t0 = Instant::now();
        renderer.render(&mut canvas, &engine.frame(), &cfg);
        render_time += t0.elapsed();
        frames += 1;
        if frames % 90 == 0 {
            let (pan, tilt) = engine.head_target();
            window.set_title(&format!(
                "Femto simulator · {:?} · render {:.1} ms · head pan {pan:+.0}° tilt {tilt:+.0}°",
                engine.resolve_emotion(),
                render_time.as_secs_f32() * 1000.0 / 90.0
            ));
            render_time = Duration::ZERO;
        }

        for (d, &p) in buf.iter_mut().zip(canvas.pixels()) {
            let (r, g, b) = femto_render::canvas::unpack(p);
            *d = (r as u32) << 16 | (g as u32) << 8 | b as u32;
        }
        // Body LEDs.
        let pal = femto_render::color::Palette::new(cfg.accent);
        let c = |x: femto_render::color::Rgb| (x.0, x.1, x.2);
        let frame = engine.frame();
        let leds = femto_core::leds::frame(&femto_core::leds::LedInput {
            ms: started.elapsed().as_millis() as u64,
            screen: &frame.screen,
            em: frame.em,
            usage: &frame.usage,
            progress: frame.progress,
            mic: if frame.em == femto_core::Emotion::Listening { 0.5 + 0.5 * (started.elapsed().as_secs_f32() * 7.0).sin() } else { 0.0 },
            speak: frame.p.mo / 6.0,
            mode: cfg.led_mode,
            brightness: cfg.led_brightness,
            flip: cfg.led_flip,
            accent: c(pal.a),
            ink: c(pal.ink),
            toxic: c(pal.toxic),
        });
        for y in H..H + LED_ROW {
            for x in 0..W {
                buf[y * W + x] = 0x101010;
            }
        }
        for (k, &(r, g, b)) in leds.iter().enumerate() {
            // Undo the LED gamma so the preview shows perceived brightness.
            let ungamma = |v: u8| ((v as f32 / 255.0).powf(1.0 / 2.2) * 255.0) as u32;
            let x0 = if k < 6 { 20 + k * 22 } else { 172 + (k - 6) * 22 };
            for y in H + 4..H + 12 {
                for x in x0..x0 + 16 {
                    buf[y * W + x] = ungamma(r) << 16 | ungamma(g) << 8 | ungamma(b);
                }
            }
        }
        window.update_with_buffer(&buf, W, H + LED_ROW).expect("present");
    }
}
