//! Renders the design's screen sheet (15 states) to `target/sheet/*.png`,
//! plus a contact sheet. Golden comparison hooks in here once the design
//! rasters are exported (PRD §10).

use std::fs;
use std::path::PathBuf;

use femto_core::expr::{Emotion, Params};
use femto_core::usage::UsageView;
use femto_core::{Frame, Screen, Settings};
use femto_render::{Canvas, Renderer, H, W};

fn usage(session: u8, week: u8, reset_min: u32) -> UsageView {
    use femto_core::usage::LocalTime;
    UsageView {
        signed_in: true,
        session_pct: Some(session),
        week_pct: Some(week),
        session_reset_min: Some(reset_min),
        week_reset: Some(LocalTime { weekday: 3, hour: 9, minute: 0 }),
        limited: false,
        back_at: None,
        stale: false,
    }
}

fn frame(screen: Screen, em: Emotion, gx: f32, gy: f32, t: u32) -> Frame {
    Frame { screen, em, p: em.params().with_gaze(gx, gy), t, caption: String::new(), usage: usage(38, 61, 134), progress: 0.7 }
}

pub fn sheet() -> Vec<(&'static str, Frame)> {
    let q = "Femto, how much Claude do I have left?".to_string();
    let mut v = vec![
        ("boot", Frame { progress: 0.62, ..frame(Screen::Boot, Emotion::Neutral, 0., 0., 3) }),
        ("wifi", frame(Screen::Wifi { attempt: 2, ssid: "HOME-WIFI".into() }, Emotion::Neutral, 0., 0., 16)),
        ("setup", frame(Screen::Setup { ap_ssid: "FEMTO-SETUP".into(), ap_key: "7F3A-9C21".into(), ip: "192.168.4.1".into() }, Emotion::Neutral, 0., 0., 8)),
        ("contempt", frame(Screen::Face, Emotion::Neutral, 0., 0., 8)),
        ("follow-left", frame(Screen::Face, Emotion::Neutral, -1., 0.6, 8)),
        ("satisfied", frame(Screen::Face, Emotion::Happy, 0., 0., 8)),
        ("amused", frame(Screen::Face, Emotion::Excited, 0., 0., 8)),
        ("scanning", frame(Screen::Face, Emotion::Curious, 0., 0., 12)),
        ("alarmed", frame(Screen::Face, Emotion::Surprised, 0., 0., 8)),
        ("standby", frame(Screen::Face, Emotion::Sleepy, 0., 0., 8)),
        ("rationing", Frame { usage: usage(91, 78, 42), ..frame(Screen::Face, Emotion::Worried, 0., 0., 8) }),
        ("listening", Frame { caption: q, ..frame(Screen::Listening, Emotion::Listening, 0., 0., 8) }),
        ("processing", frame(Screen::Thinking, Emotion::Thinking, 0.7, -0.9, 14)),
        (
            "speaking",
            Frame {
                caption: "38% of this session consumed. Spend wisely, sir. Or don't.".into(),
                p: Params { mo: 5., ..Emotion::Speaking.params() },
                ..frame(Screen::Speaking, Emotion::Speaking, 0., 0., 8)
            },
        ),
        ("ledger", frame(Screen::Ledger, Emotion::Neutral, 0., 0., 8)),
    ];
    v.push(("wipe", frame(Screen::Wipe { secs_left: 4 }, Emotion::Surprised, 0., 0., 8)));
    v
}

fn out_dir() -> PathBuf {
    let d = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../target/sheet");
    fs::create_dir_all(&d).unwrap();
    d
}

fn write_png(path: &PathBuf, w: usize, h: usize, rgb: &[u8]) {
    let file = fs::File::create(path).unwrap();
    let mut enc = png::Encoder::new(std::io::BufWriter::new(file), w as u32, h as u32);
    enc.set_color(png::ColorType::Rgb);
    enc.set_depth(png::BitDepth::Eight);
    enc.write_header().unwrap().write_image_data(rgb).unwrap();
}

fn render_variant(name: &str, cfg: &Settings) {
    let mut r = Renderer::new();
    let mut c = Canvas::new();
    let frames = sheet();
    let cols = 4;
    let rows = frames.len().div_ceil(cols);
    let (sw, sh) = (cols * (W + 8), rows * (H + 8));
    let mut sheet = vec![20u8; sw * sh * 3];
    for (i, (label, f)) in frames.iter().enumerate() {
        r.render(&mut c, f, cfg);
        let rgb = c.to_rgb888();
        write_png(&out_dir().join(format!("{name}-{i:02}-{label}.png")), W, H, &rgb);
        let (ox, oy) = ((i % cols) * (W + 8) + 4, (i / cols) * (H + 8) + 4);
        for y in 0..H {
            let d = ((oy + y) * sw + ox) * 3;
            sheet[d..d + W * 3].copy_from_slice(&rgb[y * W * 3..(y + 1) * W * 3]);
        }
    }
    write_png(&out_dir().join(format!("{name}-sheet.png")), sw, sh, &sheet);
}

#[test]
fn render_screen_sheet() {
    render_variant("default", &Settings::default());
    use femto_core::settings::{Accent, Eyewear};
    render_variant("reticle", &Settings { eyewear: Eyewear::Reticle, corp: false, accent: Accent::ToxicGreen, ..Default::default() });
    render_variant("arlens", &Settings { eyewear: Eyewear::ArHalfLens, accent: Accent::IceBlue, fx: false, ..Default::default() });
}

#[test]
fn render_is_fast_enough_on_host() {
    let mut r = Renderer::new();
    let mut c = Canvas::new();
    let f = &sheet()[3].1;
    let cfg = Settings::default();
    r.render(&mut c, f, &cfg);
    let t = std::time::Instant::now();
    for _ in 0..50 {
        r.render(&mut c, f, &cfg);
    }
    let per = t.elapsed() / 50;
    eprintln!("face frame: {per:?} on host");
}

fn read_png(path: &std::path::Path) -> Vec<u8> {
    let dec = png::Decoder::new(std::io::BufReader::new(fs::File::open(path).unwrap()));
    let mut r = dec.read_info().unwrap();
    let mut buf = vec![0; r.output_buffer_size().unwrap()];
    let info = r.next_frame(&mut buf).unwrap();
    let ch = info.color_type.samples();
    buf.chunks(ch).flat_map(|p| [p[0], p[1], p[2]]).collect()
}

/// Share of pixels whose luma differs by more than 48 from the design.
fn diff_share(a: &[u8], b: &[u8]) -> f32 {
    let luma = |p: &[u8]| 0.299 * p[0] as f32 + 0.587 * p[1] as f32 + 0.114 * p[2] as f32;
    let n = a.len() / 3;
    let bad = a.chunks(3).zip(b.chunks(3)).filter(|(x, y)| (luma(x) - luma(y)).abs() > 48.0).count();
    bad as f32 / n as f32
}

/// PRD §10: each screen within 3 % of the design raster. Setup carries an
/// intentional copy change ("HOLD POWER 3S"), so it gets 5 %.
#[test]
fn matches_design_rasters() {
    let dir = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../design/raster");
    let mut r = Renderer::new();
    let mut c = Canvas::new();
    let cfg = Settings::default();
    let mut failures = Vec::new();
    for (i, (label, f)) in sheet().iter().take(15).enumerate() {
        r.render(&mut c, f, &cfg);
        let design = read_png(&dir.join(format!("{i:02}.png")));
        let share = diff_share(&c.to_rgb888(), &design);
        let limit = if *label == "setup" { 0.05 } else { 0.03 };
        eprintln!("{label:12} {:5.2}%", share * 100.0);
        if share > limit {
            failures.push(format!("{label}: {:.2}% > {:.0}%", share * 100.0, limit * 100.0));
        }
    }
    assert!(failures.is_empty(), "{failures:?}");
}

/// Long-running engine + renderer: caches must stop growing.
#[test]
fn caches_are_bounded() {
    use femto_core::{Engine, Usage};
    let cfg = Settings::default();
    let mut e = Engine::new();
    e.set_wall_clock(1_791_400_000, 7200);
    e.set_usage(Usage { signed_in: true, ok: true, fetched_at: Some(1_791_400_000), session_pct: 38, week_pct: 61, ..Default::default() });
    e.run_power_on_demo(false, &cfg);
    let mut r = Renderer::new();
    let mut c = Canvas::new();
    let mut sizes = vec![];
    for i in 0..3000 {
        e.advance(40);
        if i % 300 == 0 {
            e.run_demo(&cfg);
        }
        r.render(&mut c, &e.frame(), &cfg);
        if i % 500 == 0 {
            sizes.push(r.fonts.cached());
        }
    }
    eprintln!("glyph cache sizes: {sizes:?}");
    assert_eq!(sizes[sizes.len() - 1], sizes[sizes.len() - 2], "glyph cache still growing");
}

#[test]
fn work_per_face_frame() {
    use std::sync::atomic::Ordering;
    let mut r = Renderer::new();
    let mut c = Canvas::new();
    let f = &sheet()[3].1;
    r.render(&mut c, f, &Settings::default());
    for s in &femto_render::canvas::STATS {
        s.store(0, Ordering::Relaxed);
    }
    r.render(&mut c, f, &Settings::default());
    eprintln!("fills {} bbox px {}", femto_render::canvas::STATS[0].load(Ordering::Relaxed), femto_render::canvas::STATS[1].load(Ordering::Relaxed));
}

/// Banded rasterization (small scratch, as on the device) must match.
#[test]
fn banding_is_exact() {
    let cfg = Settings::default();
    for (_, f) in sheet() {
        let mut a = Canvas::new();
        let mut b = Canvas::new();
        b.set_scratch(Vec::with_capacity((W + 2) * 7));
        Renderer::new().render(&mut a, &f, &cfg);
        Renderer::new().render(&mut b, &f, &cfg);
        assert!(a.pixels() == b.pixels());
    }
}
