//! The 12 body LEDs (two strips of 6: 0–5 left, 6–11 right), as a second,
//! glanceable display that follows the screen (PRD §5.6).
//!
//! Idle (Contempt / Satisfied) in `Usage` mode: session usage on the left
//! strip, weekly on the right, coloured like the status band. Everything
//! else is a mood animation. Output is gamma-corrected and scaled by the
//! brightness setting, ready for the PY32.

use crate::engine::Screen;
use crate::expr::Emotion;
use crate::settings::LedMode;
use crate::usage::{Level, UsageView};

pub const COUNT: usize = 12;
const STRIP: usize = 6;

pub type Rgb = (u8, u8, u8);

/// Everything the animation depends on.
#[derive(Clone, Debug)]
pub struct LedInput<'a> {
    pub ms: u64,
    pub screen: &'a Screen,
    pub em: Emotion,
    pub usage: &'a UsageView,
    /// Boot progress 0..1.
    pub progress: f32,
    /// Microphone level 0..1 (Listening VU).
    pub mic: f32,
    /// Speaker envelope 0..1 (Speaking).
    pub speak: f32,
    pub mode: LedMode,
    /// 0–100.
    pub brightness: u8,
    pub flip: bool,
    /// Palette, linear-ish sRGB from the renderer: accent, bone, toxic.
    pub accent: Rgb,
    pub ink: Rgb,
    pub toxic: Rgb,
}

/// Voice-turn status colours (fixed, not from the palette).
const LISTEN: (f32, f32, f32) = (0.0, 0.75, 1.0);
const THINK: (f32, f32, f32) = (1.0, 0.5, 0.0);
const SPEAK: (f32, f32, f32) = (0.7, 0.15, 1.0);

/// Colours for the 12 LEDs.
pub fn frame(i: &LedInput) -> [Rgb; COUNT] {
    let mut px = [(0.0f32, 0.0f32, 0.0f32); COUNT];
    let t = i.ms as f32 / 1000.0;
    let a = f(i.accent);
    let ink = f(i.ink);
    let set = |px: &mut [(f32, f32, f32); COUNT], k: usize, c: (f32, f32, f32), v: f32| px[k] = scale(c, v.clamp(0.0, 1.0));
    // Position k (0 = start of a strip) on both strips, mirrored.
    let both = |px: &mut [(f32, f32, f32); COUNT], k: usize, c: (f32, f32, f32), v: f32| {
        set(px, k, c, v);
        set(px, STRIP + k, c, v);
    };

    if i.mode == LedMode::Off {
        return [(0, 0, 0); COUNT];
    }
    match i.screen {
        Screen::Boot => {
            let lit = i.progress * STRIP as f32;
            for k in 0..STRIP {
                both(&mut px, k, ink, (lit - k as f32).clamp(0.0, 1.0) * 0.6);
            }
        }
        Screen::Wifi { .. } => {
            // Slow chase in bone: searching the grid.
            let head = (t * 4.0) % STRIP as f32;
            for k in 0..STRIP {
                both(&mut px, k, ink, 0.5 * trail(head, k as f32, STRIP as f32));
            }
        }
        Screen::Setup { .. } => {
            let v = if (t * 1.2).fract() < 0.5 { 0.6 } else { 0.08 };
            for k in 0..COUNT {
                set(&mut px, k, f(i.toxic), v);
            }
        }
        Screen::Wipe { .. } => {
            let v = if (t * 4.0).fract() < 0.5 { 1.0 } else { 0.0 };
            for k in 0..COUNT {
                set(&mut px, k, a, v);
            }
        }
        _ => mood(&mut px, i, t, a, ink),
    }
    finish(px, i)
}

fn mood(px: &mut [(f32, f32, f32); COUNT], i: &LedInput, t: f32, a: (f32, f32, f32), ink: (f32, f32, f32)) {
    let all = |px: &mut [(f32, f32, f32); COUNT], c: (f32, f32, f32), v: f32| {
        for p in px.iter_mut() {
            *p = scale(c, v.clamp(0.0, 1.0));
        }
    };
    match i.em {
        Emotion::Sleepy | Emotion::Dormant => {}
        // Voice turn: one colour per state, so it reads at a glance and
        // never looks like a mood or a usage alert (those use the palette).
        Emotion::Listening => {
            // Steady cyan, clearly on; brightens up the strip with the voice.
            let lit = (i.mic.sqrt() * STRIP as f32 * 1.2).min(STRIP as f32);
            for k in 0..STRIP {
                let v = 0.5 + 0.5 * (lit - k as f32).clamp(0.0, 1.0);
                px[k] = scale(LISTEN, v);
                px[STRIP + k] = scale(LISTEN, v);
            }
        }
        Emotion::Thinking => {
            // Two amber lights chasing around both strips (a loop of 12).
            let head = (t * 8.0) % COUNT as f32;
            for k in 0..COUNT {
                // Left strip forward, right strip backward: a closed loop.
                let pos = if k < STRIP { k } else { COUNT - 1 - (k - STRIP) } as f32;
                let v = trail(head, pos, COUNT as f32).max(trail((head + COUNT as f32 / 2.0) % COUNT as f32, pos, COUNT as f32));
                px[k] = scale(THINK, 0.05 + 0.95 * v);
            }
        }
        // Violet, pulsing with Femto's voice (never fully dark mid-reply).
        Emotion::Speaking => all(px, SPEAK, 0.2 + 0.8 * i.speak),
        Emotion::Curious => {
            // Sweep back and forth, both strips together.
            let ph = (t * 1.6).fract();
            let head = if ph < 0.5 { ph * 2.0 } else { 2.0 - ph * 2.0 } * (STRIP - 1) as f32;
            for k in 0..STRIP {
                let v = (1.0 - (k as f32 - head).abs()).max(0.0);
                px[k] = scale(a, 0.05 + 0.95 * v);
                px[STRIP + k] = scale(a, 0.05 + 0.95 * v);
            }
        }
        Emotion::Surprised => {
            // Three hard strobes, then hold bright.
            let ph = t % 1.6;
            let v = if ph < 0.6 { if (ph * 10.0) as u32 % 2 == 0 { 1.0 } else { 0.0 } } else { 0.8 };
            all(px, if ph < 0.6 { ink } else { a }, v);
        }
        Emotion::Excited => {
            // Sparks: each LED flashes on its own pseudo-random beat.
            for (k, p) in px.iter_mut().enumerate() {
                let beat = hash(k as u32, (t * 6.0) as u32);
                let v = if beat % 5 == 0 { 1.0 } else { 0.08 };
                *p = scale(if beat % 7 == 0 { ink } else { a }, v);
            }
        }
        Emotion::Worried => {
            // Failing neon: mostly on, irregular dropouts.
            for (k, p) in px.iter_mut().enumerate() {
                let beat = hash(k as u32 / 3, (t * 12.0) as u32);
                let v = if beat % 9 == 0 { 0.05 } else if beat % 4 == 0 { 0.5 } else { 0.85 };
                *p = scale(a, v);
            }
        }
        Emotion::Neutral | Emotion::Happy => {
            if i.mode == LedMode::Usage && i.usage.signed_in {
                meter(px, 0, i.usage.session_pct, i, t);
                meter(px, STRIP, i.usage.week_pct, i, t);
            } else {
                // Dim accent glow, breathing slowly.
                let v = 0.12 + 0.08 * (t * std::f32::consts::TAU / 6.0).sin();
                all(px, a, v);
            }
        }
    }
    let _ = ink;
}

/// A 6-LED usage meter: lit segments in the level colour, the leading one
/// breathing; unlit segments faintly on so the strip reads as a gauge.
fn meter(px: &mut [(f32, f32, f32); COUNT], base: usize, pct: Option<u8>, i: &LedInput, t: f32) {
    let Some(pct) = pct else {
        for k in 0..STRIP {
            px[base + k] = scale(f(i.ink), 0.04);
        }
        return;
    };
    let c = match Level::of(pct) {
        Level::Ok => f(i.ink),
        Level::Caution => f(i.toxic),
        Level::Alert => f(i.accent),
    };
    let lit = pct as f32 / 100.0 * STRIP as f32;
    let full = lit.floor() as usize;
    let breathe = 0.55 + 0.45 * (t * std::f32::consts::TAU / 3.0).sin();
    for k in 0..STRIP {
        px[base + k] = if k < full {
            scale(c, 0.55)
        } else if k == full && lit > full as f32 {
            // Partial top segment: brightness by fraction, breathing.
            scale(c, (0.2 + 0.6 * (lit - full as f32)) * breathe)
        } else {
            scale(c, 0.03)
        };
    }
    if pct > 0 && full == 0 && lit <= 0.0 {
        px[base] = scale(c, 0.1);
    }
}

/// Brightness of LED at `pos` for a chase head at `head` with a short tail
/// (wrapping over `len`).
fn trail(head: f32, pos: f32, len: f32) -> f32 {
    let d = (head - pos).rem_euclid(len);
    if d < 1.0 {
        1.0 - d * 0.3
    } else {
        (0.7 - (d - 1.0) * 0.35).max(0.0)
    }
}

fn hash(a: u32, b: u32) -> u32 {
    let mut x = a.wrapping_mul(0x9E37_79B9) ^ b.wrapping_mul(0x85EB_CA6B);
    x ^= x >> 13;
    x = x.wrapping_mul(0xC2B2_AE35);
    x ^ (x >> 16)
}

fn f(c: Rgb) -> (f32, f32, f32) {
    (c.0 as f32 / 255.0, c.1 as f32 / 255.0, c.2 as f32 / 255.0)
}

fn scale(c: (f32, f32, f32), v: f32) -> (f32, f32, f32) {
    (c.0 * v, c.1 * v, c.2 * v)
}

/// Flip, global brightness, gamma: perceived brightness tracks the values
/// above, and the brightness setting can't be overridden by an animation.
fn finish(px: [(f32, f32, f32); COUNT], i: &LedInput) -> [Rgb; COUNT] {
    let gain = i.brightness.min(100) as f32 / 100.0;
    let mut out = [(0u8, 0u8, 0u8); COUNT];
    for (k, o) in out.iter_mut().enumerate() {
        let src = if i.flip { (k / STRIP) * STRIP + (STRIP - 1 - k % STRIP) } else { k };
        let (r, g, b) = px[src];
        let ch = |x: f32| ((x * gain).clamp(0.0, 1.0).powf(2.2) * 255.0 + 0.5) as u8;
        *o = (ch(r), ch(g), ch(b));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn input<'a>(screen: &'a Screen, em: Emotion, usage: &'a UsageView) -> LedInput<'a> {
        LedInput {
            ms: 1000,
            screen,
            em,
            usage,
            progress: 0.5,
            mic: 0.0,
            speak: 0.0,
            mode: LedMode::Usage,
            brightness: 100,
            flip: false,
            accent: (230, 40, 50),
            ink: (230, 225, 215),
            toxic: (220, 230, 60),
        }
    }

    fn usage(s: u8, w: u8) -> UsageView {
        UsageView { signed_in: true, session_pct: Some(s), week_pct: Some(w), ..UsageView::none() }
    }

    #[test]
    fn usage_meter_fills_by_percent_and_colours_by_level() {
        let u = usage(50, 90);
        let px = frame(&input(&Screen::Face, Emotion::Neutral, &u));
        // Session 50 %: 3 of 6 lit (bone), rest dim.
        assert!(px[0].0 > 40 && px[2].0 > 40); // 0.55 after gamma 2.2 ≈ 55
        assert!(px[4].0 < 10);
        // Week 90 %: 5 lit, accent red (red channel dominates).
        assert!(px[6].0 > 3 * px[6].1.max(1));
        assert!(px[10].0 > 0);
    }

    #[test]
    fn standby_and_off_are_dark() {
        let u = usage(50, 50);
        assert!(frame(&input(&Screen::Face, Emotion::Sleepy, &u)).iter().all(|p| *p == (0, 0, 0)));
        let mut i = input(&Screen::Face, Emotion::Neutral, &u);
        i.mode = LedMode::Off;
        assert!(frame(&i).iter().all(|p| *p == (0, 0, 0)));
    }

    #[test]
    fn brightness_scales_down() {
        let u = usage(100, 100);
        let mut i = input(&Screen::Face, Emotion::Neutral, &u);
        let full = frame(&i)[0].0;
        i.brightness = 30;
        assert!(frame(&i)[0].0 < full / 4);
    }

    #[test]
    fn listening_vu_follows_mic() {
        let u = usage(0, 0);
        let mut i = input(&Screen::Listening, Emotion::Listening, &u);
        i.mic = 0.0;
        let quiet: u32 = frame(&i).iter().map(|p| p.2 as u32).sum();
        i.mic = 0.8;
        let loud: u32 = frame(&i).iter().map(|p| p.2 as u32).sum();
        assert!(loud > quiet * 3 / 2);
    }

    #[test]
    fn voice_states_have_distinct_colours() {
        let u = usage(0, 0);
        let lit = |em: Emotion, screen: &Screen| -> Rgb {
            let mut i = input(screen, em, &u);
            i.speak = 0.5;
            // The brightest LED of the frame.
            frame(&i).into_iter().max_by_key(|p| p.0 as u32 + p.1 as u32 + p.2 as u32).unwrap()
        };
        let (l, t, s) = (lit(Emotion::Listening, &Screen::Listening), lit(Emotion::Thinking, &Screen::Thinking), lit(Emotion::Speaking, &Screen::Speaking));
        // Listening: blue dominant; thinking: red > blue (amber); speaking: blue > green (violet).
        assert!(l.2 > l.0 && l.1 > l.0, "listening {l:?}");
        assert!(t.0 > t.2 && t.1 > t.2, "thinking {t:?}");
        assert!(s.2 > s.1 && s.0 > s.1, "speaking {s:?}");
        // Listening is visibly on even in silence.
        let mut i = input(&Screen::Listening, Emotion::Listening, &u);
        i.mic = 0.0;
        assert!(frame(&i).iter().all(|p| p.2 > 30), "{:?}", frame(&i));
    }

    #[test]
    fn flip_mirrors_each_strip() {
        let u = usage(34, 34);
        let mut i = input(&Screen::Face, Emotion::Neutral, &u);
        let a = frame(&i);
        i.flip = true;
        let b = frame(&i);
        assert_eq!(a[0], b[5]);
        assert_eq!(a[6], b[11]);
    }
}
