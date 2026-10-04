//! Every screen from the design's screen sheet, ported from its SVG code
//! (`face`, `status`, `ledger`, `boot`, `wifi`, `setup`, `screen`).
//! Coordinates and constants are the design's, verbatim.

use femto_core::expr::{Emotion, Params};
use femto_core::settings::{Eyewear, Settings};
use femto_core::text::{ledger_verdict, wrap_last_two};
use femto_core::usage::{fmt_minutes, Level, UsageView};
use femto_core::{Frame, Screen};

use crate::canvas::{Canvas, Xf, H, W};
use crate::color::{Palette, Rgb};
use crate::path::Path;
use crate::text::{Face, Fonts, Style};

/// Draws frames. Holds the font cache and the palette for the current accent.
pub struct Renderer {
    pub fonts: Fonts,
    /// Microseconds spent in: background, content, chrome, overlay (last frame).
    pub timings: [u32; 4],
    pal: Palette,
    /// Background pre-rendered for the current accent.
    bg: Option<Vec<u16>>,
    /// Face screens: background + status band + footer, re-rendered only
    /// when what they show changes (key).
    base: Option<(String, Vec<u16>)>,
    shade: Vec<u8>,
    accent: femto_core::settings::Accent,
}

impl Default for Renderer {
    fn default() -> Self {
        Renderer::new()
    }
}

struct Ctx<'a> {
    c: &'a mut Canvas,
    f: &'a mut Fonts,
    p: Palette,
    cfg: &'a Settings,
}

/// Tag options (the design's `tag()`).
#[derive(Clone, Copy)]
struct Tag {
    size: f32,
    bg: Option<Rgb>,
    fg: Option<Rgb>,
    stroke: Option<Rgb>,
    center: bool,
    face: Face,
}

impl Tag {
    fn new(size: f32) -> Tag {
        Tag { size, bg: None, fg: None, stroke: None, center: false, face: Face::BoldItalic }
    }
}

impl Renderer {
    pub fn new() -> Renderer {
        let accent = Default::default();
        Renderer { fonts: Fonts::new(), timings: [0; 4], pal: Palette::new(accent), bg: None, base: None, shade: overlay_shade(), accent }
    }

    pub fn render(&mut self, c: &mut Canvas, frame: &Frame, cfg: &Settings) {
        if cfg.accent != self.accent {
            self.accent = cfg.accent;
            self.pal = Palette::new(cfg.accent);
            self.bg = None;
            self.base = None;
        }
        let pal = self.pal;
        let bg = self.bg.get_or_insert_with(|| {
            let mut tmp = Canvas::new();
            background(&mut tmp, &pal);
            tmp.pixels().to_vec()
        });
        let t0 = std::time::Instant::now();
        let (y0, rows) = c.window();
        let full = y0 == 0 && rows == H;
        let face_screen = !matches!(frame.screen, Screen::Ledger | Screen::Boot | Screen::Wifi { .. } | Screen::Setup { .. } | Screen::Wipe { .. });
        let has_caption = !frame.caption.is_empty();
        let mood = match frame.screen {
            Screen::Speaking => cfg.name.to_uppercase(),
            _ => frame.em.label().to_uppercase(),
        };
        let footer = cfg.corp && !has_caption && frame.em != Emotion::Sleepy;
        let t1;
        let mut t2 = t0;
        if face_screen && full {
            // Static layer from cache; rebuild when its inputs change.
            let key = format!("{mood}|{:?}|{footer}|{}|{:?}", frame.usage, cfg.corp_name, cfg.accent);
            if self.base.as_ref().is_none_or(|(k, _)| *k != key) {
                c.pixels_mut().copy_from_slice(bg);
                let mut x = Ctx { c: &mut *c, f: &mut self.fonts, p: self.pal, cfg };
                x.c.xf = Xf::ID;
                x.c.alpha = 1.0;
                x.status(&frame.usage, &mood);
                if footer {
                    let s = format!("PROPERTY OF {} · EMP-0007", cfg.corp_name.to_uppercase());
                    x.text(160., 233., &s, x.mono(8., x.p.sec).middle());
                }
                self.base = Some((key, c.pixels().to_vec()));
            } else {
                c.pixels_mut().copy_from_slice(&self.base.as_ref().unwrap().1);
            }
            t1 = std::time::Instant::now();
            let mut x = Ctx { c: &mut *c, f: &mut self.fonts, p: self.pal, cfg };
            x.c.xf = Xf::ID;
            x.c.alpha = 1.0;
            x.face(&frame.p, frame.em, frame.t);
            t2 = std::time::Instant::now();
            if has_caption {
                x.caption(frame.screen == Screen::Listening, &frame.caption);
            }
        } else {
            c.pixels_mut().copy_from_slice(&bg[y0 * W..(y0 + rows) * W]);
            t1 = std::time::Instant::now();
            let mut x = Ctx { c: &mut *c, f: &mut self.fonts, p: self.pal, cfg };
            x.c.xf = Xf::ID;
            x.c.alpha = 1.0;
            match &frame.screen {
                Screen::Ledger => x.ledger(&frame.usage),
                Screen::Boot => x.boot(frame.t, frame.progress),
                Screen::Wifi { attempt, ssid } => x.wifi(frame.t, *attempt, ssid),
                Screen::Setup { ap_ssid, ap_key, ip } => x.setup(frame.t, ap_ssid, ap_key, ip),
                Screen::Wipe { secs_left } => x.wipe(frame.t, *secs_left),
                screen => {
                    x.face(&frame.p, frame.em, frame.t);
                    t2 = std::time::Instant::now();
                    x.status(&frame.usage, &mood);
                    if footer {
                        let s = format!("PROPERTY OF {} · EMP-0007", cfg.corp_name.to_uppercase());
                        x.text(160., 233., &s, x.mono(8., x.p.sec).middle());
                    }
                    if has_caption {
                        x.caption(*screen == Screen::Listening, &frame.caption);
                    }
                }
            }
            if t2 == t0 {
                t2 = std::time::Instant::now();
            }
        }
        let t3 = std::time::Instant::now();
        // Vignette + scanlines are applied at scan-out (Canvas shade).
        if cfg.fx != c_has_shade(c) {
            c.set_shade(cfg.fx.then(|| self.shade.clone()));
        }
        let us = |a: std::time::Instant, b: std::time::Instant| (b - a).as_micros() as u32;
        self.timings = [us(t0, t1), us(t1, t2), us(t2, t3), us(t3, std::time::Instant::now())];
    }
}

impl Ctx<'_> {
    // ---- helpers ------------------------------------------------------------

    fn text(&mut self, x: f32, y: f32, s: &str, st: Style) {
        self.c.text(self.f, x, y, s, st);
    }

    /// System readouts. Small sizes use the Terminus bitmap font: at
    /// 7–12 px an anti-aliased outline font is unreadable on the 2" panel.
    fn mono(&self, size: f32, color: Rgb) -> Style {
        // Mini faces (boot/setup) keep the tiny outline text: a full-size
        // bitmap there would cover the little face.
        if self.c.xf.s < 0.75 {
            return Style::new(Face::Mono, size, color);
        }
        let face = match size * self.c.xf.s {
            s if s <= 10.5 => Face::Term12,
            s if s <= 12.5 => Face::Term12B,
            s if s <= 15.0 => Face::Term14B,
            _ => Face::Mono,
        };
        Style::new(face, size, color)
    }

    fn heavy(&self, size: f32, color: Rgb) -> Style {
        Style::new(Face::BlackItalic, size, color)
    }

    fn rect(&mut self, x: f32, y: f32, w: f32, h: f32, c: Rgb, op: f32) {
        self.c.fill(&Path::rect(x, y, w, h), c, op);
    }

    fn lvl(&self, pct: u8) -> Rgb {
        match Level::of(pct) {
            Level::Ok => self.p.ink,
            Level::Caution => self.p.toxic,
            Level::Alert => self.p.a,
        }
    }

    /// Label with a slanted right edge.
    fn tag(&mut self, x: f32, y: f32, s: &str, o: Tag) {
        let mono = o.face == Face::Mono;
        let w = s.chars().count() as f32 * o.size * if mono { 0.55 } else { 0.46 } + 14.0;
        let hh = o.size + 8.0;
        let x0 = if o.center { x - w / 2.0 } else { x };
        let shape = Path::polygon(&[(x0, y), (x0 + w + 6.0, y), (x0 + w, y + hh), (x0, y + hh)]);
        self.c.fill(&shape, o.bg.unwrap_or(self.p.panel), 1.0);
        if let Some(st) = o.stroke {
            self.c.stroke(&shape, 1.0, st, 1.0);
        }
        let st = Style::new(o.face, o.size, o.fg.unwrap_or(self.p.ink));
        self.text(x0 + 7.0, y + hh - 5.5, s, st);
    }

    /// Text drawn twice: accent offset by `dx`, then `fg` on top.
    fn misregistered(&mut self, x: f32, y: f32, dx: f32, s: &str, st: Style, op: f32) {
        let mut back = st;
        back.color = self.p.a;
        back.opacity = op;
        self.text(x + dx, y, s, back);
        self.text(x, y, s, st);
    }

    fn hazard(&mut self, y: f32) {
        let a = self.p.a;
        let y0 = y as usize;
        self.c.effect(0, y0, W, y0 + 4, |x, y| {
            let u = (x as f32 + 0.5 + y as f32 + 0.5) / core::f32::consts::SQRT_2;
            (u.rem_euclid(10.0) < 5.0).then_some((a, 1.0))
        });
    }

    fn band(&mut self, h: f32) {
        self.rect(0., 0., 320., h, self.p.panel, 1.0);
        self.hazard(h);
    }

    // ---- layers -------------------------------------------------------------

    // ---- face ---------------------------------------------------------------

    fn mini(&mut self, x: f32, y: f32, sc: f32, p: &Params, em: Emotion, t: u32) {
        let xf = Xf { s: sc, tx: x - 160.0 * sc, ty: y - 125.0 * sc };
        self.c.with_xf(xf, |c| {
            let mut sub = Ctx { c, f: &mut *self.f, p: self.p, cfg: self.cfg };
            sub.face(p, em, t);
        });
    }

    fn face(&mut self, p: &Params, em: Emotion, t: u32) {
        let fx = self.cfg.fx;
        let (ox, oy) = (p.gx, p.gy);
        let cy = 116.0 + oy * 4.0;
        let lx = 112.0 + ox * 5.0;
        let rx = 208.0 + ox * 5.0;
        let blink = em.blinks() && t % 55 < 2;
        let glitch = fx && (t % 60 < 2 || (em == Emotion::Worried && t % 14 < 2));
        let shift = if glitch { 7.0 } else { 0.0 };
        self.c.with_xf(Xf { s: 1.0, tx: shift, ty: 0.0 }, |c| {
            let mut g = Ctx { c, f: &mut *self.f, p: self.p, cfg: self.cfg };
            g.face_body(p, em, t, lx, rx, cy, blink);
        });
        if glitch {
            let (a, ink, bg) = (self.p.a, self.p.ink, self.p.bg);
            let tf = t as f32;
            self.rect(0., (tf * 37.).rem_euclid(170.) + 50., 320., 6., a, 0.6);
            self.rect(40., (tf * 53.).rem_euclid(150.) + 60., 200., 2., ink, 1.0);
            self.rect(0., (tf * 71.).rem_euclid(150.) + 60., 320., 12., bg, 0.5);
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn face_body(&mut self, p: &Params, em: Emotion, t: u32, lx: f32, rx: f32, cy: f32, blink: bool) {
        let (ox, oy) = (p.gx, p.gy);
        let hw = 34.0;
        let open = p.slp <= 0.5 && !blink;
        let pal = self.p;

        let mut tp = std::time::Instant::now();
        let mut lap = |i: usize| {
            let n = std::time::Instant::now();
            PROF[i].fetch_add((n - tp).as_micros() as u32, std::sync::atomic::Ordering::Relaxed);
            tp = n;
        };
        if self.cfg.corp {
            self.corp_marks(lx, rx, cy, p.drop);
        } else {
            self.implant(lx, rx, cy, t);
        }

        if open {
            for (ecx, top, bot) in [(lx, p.l_t, p.l_b), (rx, p.r_t, p.r_b)] {
                let eye = eye_path(ecx, cy, hw, top, bot);
                self.c.fill(&eye, pal.sclera, 1.0);
                let icx = ecx + ox * 9.0;
                let icy = cy + 2.0 + oy * 6.0;
                let (r_x, r_y) = (14.0 * p.isc, 17.0 * p.isc);
                self.c.with_clip(&eye, |c| {
                    glow_ellipse(c, icx, icy, r_x, r_y, pal.a);
                    c.fill(&Path::ellipse(icx, icy, r_x, r_y), pal.a, 1.0);
                    c.fill(&Path::ellipse(icx, icy + r_y * 0.55, r_x * 0.6, r_y * 0.28), pal.al, 1.0);
                    c.fill(&Path::ellipse(icx, icy, 2.6, 11.0 * p.isc), pal.panel, 1.0);
                    let shadow = Path::new()
                        .move_to(ecx - hw, cy)
                        .quad_to(ecx, cy - 2.0 * top, ecx + hw, cy)
                        .line_to(ecx + hw, cy + 10.0)
                        .quad_to(ecx, cy - 2.0 * top + 20.0, ecx - hw, cy + 10.0)
                        .close();
                    c.fill(&shadow, pal.panel, 0.6);
                    c.fill(&Path::circle(icx + 5.0, icy - 5.0, 2.2), pal.ink, 1.0);
                });
            }
        }

        lap(0);
        let mode = if p.slp > 0.5 { 2 } else if blink { 1 } else { 0 };
        if self.cfg.fx {
            self.c.with_xf(Xf { s: 1.0, tx: 2.5, ty: 0.0 }, |c| {
                c.with_alpha(0.7, |c| ink_layer(c, p, pal.a, pal.panel, lx, rx, cy, mode))
            });
        }
        lap(1);
        ink_layer(self.c, p, pal.ink, pal.panel, lx, rx, cy, mode);
        lap(2);

        // Scar through the left brow.
        let (sx1, sy1, sx2, sy2) = (lx - 6.0, cy + p.blo - 12.0, lx + 6.0, cy + p.bli + 14.0);
        self.c.line(sx1, sy1, sx2, sy2, 3.0, pal.bg, 1.0);
        self.c.line(sx1, sy1, sx2, sy2, 1.0, pal.a, 0.8);

        match self.cfg.eyewear {
            Eyewear::Reticle => self.reticle(rx, cy, p, em, t),
            Eyewear::None => {}
            _ => self.eyewear(lx, rx, cy, p, em, t),
        }
        lap(3);
        self.decorations(em, t);
        lap(4);
        let _ = oy;
    }

    fn corp_marks(&mut self, lx: f32, rx: f32, cy: f32, drop: f32) {
        let (x, y) = (lx - 46.0, cy + 36.0);
        let a = self.p.a;
        self.c.stroke(&Path::rect_rotated(x - 6., y - 6., 12., 12., 45., x, y), 1.5, a, 1.0);
        self.c.fill(&Path::rect_rotated(x - 2.5, y - 2.5, 5., 5., 45., x, y), a, 1.0);
        let id = format!("{}-07", self.cfg.corp_initials());
        self.text(x, y + 19., &id, self.mono(7., a).middle());
        // Hidden while the glasses slide down (their readout would cover it).
        if drop < 4.0 {
            self.text(rx + 8., cy + 45., "EMP 0007", self.mono(7., self.p.ink).opacity(0.6));
        }
    }

    fn implant(&mut self, lx: f32, rx: f32, cy: f32, t: u32) {
        let (a, dim, ink) = (self.p.a, self.p.dim, self.p.ink);
        let pts = [(lx - 38., cy + 6.), (lx - 50., cy + 18.), (lx - 50., cy + 46.), (lx - 42., cy + 54.)];
        self.c.stroke(&Path::polyline(&pts), 1.5, a, 0.85);
        self.c.fill(&Path::circle(lx - 42., cy + 54., 2.5), a, 1.0);
        self.c.fill(&Path::circle(lx - 50., cy + 30., 1.8), if t % 20 < 10 { a } else { dim }, 1.0);
        for (i, w) in [1., 2., 1., 1.5, 2.5, 1., 2.].into_iter().enumerate() {
            self.rect(rx + 10. + i as f32 * 3.2, cy + 33., w, 9., ink, 0.55);
        }
    }

    fn eyewear(&mut self, lx: f32, rx: f32, cy: f32, p: &Params, em: Emotion, t: u32) {
        let dy = p.drop * 0.5;
        let (a, ink, dim) = (self.p.a, self.p.ink, self.p.dim);
        let hud = match em {
            Emotion::Surprised => "ALERT",
            Emotion::Curious => "ID MATCH",
            Emotion::Worried => "QUOTA LOW",
            Emotion::Sleepy => "IDLE",
            _ => "MONITORING",
        };
        if self.cfg.eyewear == Eyewear::ExecGlasses {
            for cx in [lx, rx] {
                let frame = Path::rrect(cx - 42., cy - 22. + dy, 84., 40., 5.);
                self.c.fill(&frame, a, 0.06);
                self.c.stroke(&frame, 2., ink, 1.0);
                self.c.line(cx - 42., cy - 21. + dy, cx + 42., cy - 21. + dy, 5., ink, 1.0);
                self.c.line(cx + 14., cy - 16. + dy, cx + 28., cy + 2. + dy, 1.5, ink, 0.25);
                self.c.line(cx + 22., cy - 16. + dy, cx + 32., cy - 4. + dy, 1., ink, 0.2);
            }
            let bridge = Path::new().move_to(lx + 42., cy - 14. + dy).quad_to(160., cy - 24. + dy, rx - 42., cy - 14. + dy);
            self.c.stroke(&bridge, 2.5, ink, 1.0);
            self.c.line(lx - 42., cy - 19. + dy, 18., cy - 26., 2.5, ink, 1.0);
            self.c.line(rx + 42., cy - 19. + dy, 302., cy - 26., 2.5, ink, 1.0);
            // Below the lens frame (Terminus is taller than the design's 7 px).
            self.text(rx + 42., cy + 30. + dy, hud, self.mono(7., a).end());
            let led = Path::rect(rx - 42., cy + 22. + dy, 4., 4.);
            if t % 16 < 8 {
                self.c.fill(&led, a, 1.0);
            }
            self.c.stroke(&led, 1., a, 1.0);
        } else {
            self.rect(rx - 40., cy - 22. + dy, 82., 36., a, 0.12);
            self.c.stroke(&Path::rect(rx - 40., cy - 22. + dy, 82., 36.), 1., a, 1.0);
            self.rect(rx - 44., cy - 27. + dy, 92., 5., ink, 1.0);
            self.c.line(rx + 48., cy - 25. + dy, 322., cy - 30., 3., ink, 1.0);
            self.c.fill(&Path::circle(rx + 52., cy - 25. + dy, 3.), if t % 20 < 10 { a } else { dim }, 1.0);
            self.text(rx - 40., cy + 24. + dy, hud, self.mono(7., a));
            for i in 0..4 {
                let i = i as f32;
                self.rect(rx + 26. + i * 4., cy + 21. + dy - i * 2., 2.5, 3. + i * 2., a, 1.0);
            }
            self.c.line(rx + 10., cy - 18. + dy, rx + 24., cy + dy, 1.2, ink, 0.25);
        }
    }

    fn reticle(&mut self, rx: f32, cy: f32, p: &Params, em: Emotion, t: u32) {
        let a = self.p.a;
        let (mcx, mcy) = (rx, cy + 1.0);
        let r0 = 36.0 + p.drop * 0.4;
        let tf = t as f32;
        let rot = match em {
            Emotion::Curious => tf * 6.0,
            Emotion::Surprised => tf * 14.0,
            _ => tf * 1.5,
        };
        let lead = Path::polyline(&[(mcx + 30., mcy + 22.), (mcx + 52., mcy + 40.), (330., mcy + 40.)]);
        self.c.stroke(&lead, 1.5, a, 1.0);
        let ring = Path::circle(mcx, mcy, r0);
        self.c.fill(&ring, a, 0.06);
        self.c.stroke(&ring, 2., a, 1.0);
        for ang in [0., 90., 180., 270.] {
            let tick = Path::line(mcx, mcy - r0 - 6., mcx, mcy - r0 + 5.).rotated(ang, mcx, mcy);
            self.c.stroke(&tick, 2., a, 1.0);
        }
        let dash = Path::circle(mcx, mcy, r0 + 7.).rotated(rot, mcx, mcy).dashed(14., 30.);
        self.c.stroke(&dash, 1.2, a, 0.8);
        let label = if em == Emotion::Curious { "SCAN" } else { "TGT" };
        self.text(mcx + r0 - 2., mcy - r0 - 2., label, self.mono(8., a));
    }

    fn decorations(&mut self, em: Emotion, t: u32) {
        let (a, ink, panel, sec) = (self.p.a, self.p.ink, self.p.panel, self.p.sec);
        let tf = t as f32;
        match em {
            Emotion::Surprised => {
                self.text(272., 90., "!", self.heavy(50., a));
                self.text(270., 88., "!", self.heavy(50., ink));
            }
            Emotion::Curious => {
                let sy = 60. + (tf * 4.).rem_euclid(130.);
                self.rect(30., sy, 260., 1.5, a, 0.7);
                self.rect(30., sy - 8., 260., 8., a, 0.08);
            }
            Emotion::Worried if t % 10 < 6 => {
                let tag = Tag { bg: Some(a), fg: Some(panel), face: Face::Jp, ..Tag::new(12.) };
                self.tag(236., 52., "警告", tag);
            }
            Emotion::Excited => {
                for (i, (x, y)) in [(56., 76.), (270., 70.), (50., 160.), (276., 160.)].into_iter().enumerate() {
                    if (t + i as u32 * 3) % 8 < 5 {
                        let r = 5. + 3. * (tf * 0.4 + i as f32).sin().abs();
                        self.c.line(x - r, y, x + r, y, 2., a, 1.0);
                        self.c.line(x, y - r, x, y + r, 2., a, 1.0);
                    }
                }
            }
            Emotion::Sleepy => {
                let s = if t % 12 < 6 { "STANDBY _" } else { "STANDBY" };
                self.text(160., 212., s, self.mono(11., sec).middle());
            }
            Emotion::Thinking => {
                let k = (t / 4) % 6;
                for i in 0..5u32 {
                    let r = Path::rect(244. + i as f32 * 9., 64., 7., 7.);
                    if i < k {
                        self.c.fill(&r, a, 1.0);
                    }
                    self.c.stroke(&r, 1.2, a, 1.0);
                }
                self.text(244., 58., "PROC", self.mono(9., a));
            }
            Emotion::Listening => {
                for i in 0..4 {
                    let fi = i as f32;
                    let hh = 6. + 14. * (tf * 0.7 + fi * 1.3).sin().abs();
                    self.rect(26. + fi * 7., 120. - hh / 2., 4., hh, a, 1.0);
                    self.rect(270. + fi * 7., 120. - hh / 2., 4., hh, a, 1.0);
                }
            }
            _ => {}
        }
    }

    // ---- chrome -------------------------------------------------------------

    fn status(&mut self, u: &UsageView, mood: &str) {
        self.band(38.);
        let (sec, ink, dim) = (self.p.sec, self.p.ink, self.p.dim);
        let fade = if u.stale { 0.5 } else { 1.0 };
        let segs = |me: &mut Self, x: f32, pct: Option<u8>| {
            let lit = pct.map_or(0, |p| (p as f32 / 10.0).round() as u32);
            let col = pct.map_or(dim, |p| me.lvl(p));
            for i in 0..10 {
                let c = if i < lit { col } else { dim };
                me.rect(x + i as f32 * 10.2, 19., 8., 4., c, fade);
            }
        };
        let pct = |p: Option<u8>| p.map_or("--%".to_string(), |p| format!("{p}%"));
        self.text(10., 14., "SESSION", self.mono(10., sec));
        let col = u.session_pct.map_or(dim, |p| self.lvl(p));
        self.text(110., 15., &pct(u.session_pct), self.heavy(14., col).end().opacity(fade));
        segs(self, 10., u.session_pct);
        let rst = u.session_reset_min.map_or("RST --".into(), |m| format!("RST {}", fmt_minutes(m).to_uppercase()));
        self.text(10., 34., &rst, self.mono(10., ink).opacity(fade));

        self.text(210., 14., "WEEK", self.mono(10., sec));
        let col = u.week_pct.map_or(dim, |p| self.lvl(p));
        self.text(310., 15., &pct(u.week_pct), self.heavy(14., col).end().opacity(fade));
        segs(self, 210., u.week_pct);
        let wr = u.week_reset.map_or("RST --".into(), |t| format!("RST {} {:02}:{:02}", t.weekday_short(), t.hour, t.minute));
        self.text(310., 34., &wr, self.mono(10., ink).end().opacity(fade));

        let tag = Tag { bg: Some(self.p.a), fg: Some(self.p.panel), center: true, face: Face::BlackItalic, ..Tag::new(12.) };
        self.tag(160., 9., mood, tag);
    }

    fn caption(&mut self, you: bool, text: &str) {
        let (a, ink, panel) = (self.p.a, self.p.ink, self.p.panel);
        let boxp = Path::polygon(&[(6., 194.), (302., 194.), (314., 206.), (314., 236.), (6., 236.)]);
        self.c.fill(&boxp, panel, 1.0);
        self.c.stroke(&boxp, 1.2, if you { ink } else { a }, 1.0);
        let who = if you { "YOU".to_string() } else { self.cfg.name.to_uppercase() };
        let tag = Tag { bg: Some(if you { ink } else { a }), fg: Some(panel), face: Face::BlackItalic, ..Tag::new(11.) };
        self.tag(6., 182., &who, tag);
        // Wrap by measured width: Cyrillic (Fira fallback) runs wider than
        // Barlow's Latin, so a character count would overflow the box.
        let f = &mut *self.f;
        let lines = wrap_last_two(text, |l| f.measure(Face::Medium, 16., l, 0.) <= 290.);
        for (i, l) in lines.iter().enumerate() {
            self.text(14., 212. + i as f32 * 17., l, Style::new(Face::Medium, 16., ink));
        }
    }

    // ---- screens --------------------------------------------------------------

    fn ledger(&mut self, u: &UsageView) {
        let (a, sec, dim, ink) = (self.p.a, self.p.sec, self.p.dim, self.p.ink);
        self.band(34.);
        self.text(12., 24., "THE LEDGER", self.heavy(20., ink));
        let right = if self.cfg.corp {
            self.cfg.corp_name.to_uppercase()
        } else {
            format!("UNIT 07 // {}", self.cfg.name.to_uppercase())
        };
        self.text(308., 22., &right, self.mono(10., a).end());

        let session_sub = u.session_reset_min.map_or("Reset unknown".into(), |m| format!("Resets in {}", fmt_minutes(m)));
        let week_sub = u.week_reset.map_or("Reset unknown".into(), |t| format!("Resets {} {:02}:{:02}", t.weekday_long(), t.hour, t.minute));
        for (y, label, pct, sub) in [(62., "SESSION // 5H", u.session_pct, session_sub), (134., "WEEKLY // QUOTA", u.week_pct, week_sub)] {
            self.text(14., y, label, self.mono(10., sec));
            self.text(14., y + 17., &sub, Style::new(Face::SemiBold, 13., ink));
            let txt = pct.map_or("--%".into(), |p| format!("{p}%"));
            let col = pct.map_or(dim, |p| self.lvl(p));
            self.text(309., y + 20., &txt, self.heavy(40., a).end().opacity(0.8));
            self.text(306., y + 20., &txt, self.heavy(40., col).end());
            let n = pct.map_or(0, |p| (p as f32 / 100.0 * 24.0).round() as usize);
            for i in 0..24 {
                let r = Path::rect(14. + i as f32 * 12.2, y + 28., 10., 10.);
                if i < n {
                    self.c.fill(&r, col, 1.0);
                } else {
                    self.c.stroke(&r, 1.2, dim, 1.0);
                }
            }
        }
        if u.stale && u.signed_in {
            let tag = Tag { bg: Some(dim), fg: Some(ink), face: Face::Mono, ..Tag::new(10.) };
            self.tag(250., 40., "STALE", tag);
        }
        let verdict = ledger_verdict(u, self.cfg.honorific.as_str());
        self.tag(14., 202., &verdict, Tag { stroke: Some(a), ..Tag::new(15.) });
    }

    fn boot(&mut self, t: u32, prog: f32) {
        let (a, ink, dim, sec) = (self.p.a, self.p.ink, self.p.dim, self.p.sec);
        let awake = prog > 0.5;
        let (p, em) = if awake { (Params::NEUTRAL, Emotion::Neutral) } else { (Emotion::Sleepy.params(), Emotion::Dormant) };
        self.mini(160., 60., 0.42, &p.with_gaze(0., 0.), em, t);
        let name = self.cfg.name.to_uppercase();
        self.misregistered(160., 146., 3., &name, self.heavy(54., ink).middle(), 1.0);
        self.text(160., 164., "フェムト · 監視", Style::new(Face::Jp, 11., a).middle().spacing(3.));
        let n = (prog * 16.0).round() as usize;
        for i in 0..16 {
            let r = Path::rect(88. + i as f32 * 9., 174., 7., 8.);
            if i < n {
                self.c.fill(&r, ink, 1.0);
            }
            self.c.stroke(&r, 1.2, dim, 1.0);
        }
        const LINES: [&str; 4] = ["> waking the machine", "> calibrating contempt", "> syncing ledger", "> online. unfortunately for you."];
        let line = LINES[((prog * 3.999) as usize).min(3)];
        let s = if t % 8 < 4 { format!("{line}_") } else { line.to_string() };
        self.text(160., 204., &s, self.mono(12., ink).middle());
        let foot = if self.cfg.corp {
            format!("{} · ASSET 07", self.cfg.corp_name.to_uppercase())
        } else {
            format!("FW {} · UNIT 07", env!("CARGO_PKG_VERSION"))
        };
        self.text(160., 230., &foot, self.mono(10., sec).middle());
    }

    fn wifi(&mut self, t: u32, attempt: u8, ssid: &str) {
        let (a, ink, dim, sec) = (self.p.a, self.p.ink, self.p.dim, self.p.sec);
        let k = (t / 5) % 4;
        let (cx, cy) = (160.0f32, 90.0f32);
        self.c.fill(&Path::circle(cx, cy, 5.), a, 1.0);
        for (i, r) in [16.0f32, 30., 44.].into_iter().enumerate() {
            let pts: Vec<(f32, f32)> = (0..=16)
                .map(|j| {
                    let ang = (-135.0 + 90.0 * j as f32 / 16.0f32).to_radians();
                    (cx + r * ang.cos(), cy + r * ang.sin())
                })
                .collect();
            self.c.stroke(&Path::polyline(&pts), 6., if k > i as u32 { ink } else { dim }, 1.0);
        }
        self.text(160., 124., "SEARCHING THE GRID", self.mono(11., sec).middle());
        self.text(160., 156., &ssid.to_uppercase(), self.heavy(32., ink).middle());
        let at = format!("ATTEMPT {attempt}/3");
        self.text(160., 176., &at, self.mono(11., if attempt > 1 { a } else { ink }).middle());
        let quip = format!("The grid is slow tonight, {}.", self.cfg.honorific.as_str());
        self.tag(160., 192., &quip, Tag { stroke: Some(a), center: true, ..Tag::new(15.) });
    }

    fn setup(&mut self, t: u32, ssid: &str, key: &str, ip: &str) {
        let (a, ink, sec) = (self.p.a, self.p.ink, self.p.sec);
        self.band(34.);
        self.text(12., 23., "CONNECT ME. I DON'T ENJOY WAITING.", self.heavy(16., ink));
        self.text(14., 60., "01 // JOIN FROM YOUR PHONE", self.mono(10., sec));
        self.text(14., 86., ssid, self.heavy(28., ink));
        self.text(14., 103., &format!("KEY  {key}"), self.mono(11., ink));
        self.text(14., 128., "02 // THEN OPEN", self.mono(10., sec));
        self.misregistered(14., 160., 2., ip, self.heavy(34., ink), 1.0);
        let threat = if self.cfg.corp { "Unregistered assets are recycled." } else { "Unconfigured units get recycled." };
        self.tag(14., 180., threat, Tag { stroke: Some(a), ..Tag::new(15.) });
        self.text(14., 230., "HOLD POWER 3S = FACTORY WIPE", self.mono(10., sec));
        self.mini(262., 122., 0.34, &Params::NEUTRAL.with_gaze(-0.9, 0.), Emotion::Neutral, t);
    }

    fn wipe(&mut self, t: u32, secs_left: u8) {
        let (a, ink, sec) = (self.p.a, self.p.ink, self.p.sec);
        self.band(34.);
        self.text(12., 23., "WIPE ME? PRESS AGAIN.", self.heavy(16., ink));
        self.text(14., 60., "FACTORY WIPE // WI-FI · TOKENS · SETTINGS", self.mono(10., sec));
        self.misregistered(90., 150., 3., &secs_left.to_string(), self.heavy(80., ink).middle(), 1.0);
        self.text(14., 180., "PRESS POWER TO CONFIRM · TAP SCREEN TO CANCEL", self.mono(9., a));
        let hon = self.cfg.honorific.as_str();
        self.tag(14., 196., &format!("Everything I knew about you, {hon}. Gone."), Tag { stroke: Some(a), ..Tag::new(15.) });
        self.mini(232., 112., 0.42, &Emotion::Surprised.params(), Emotion::Surprised, t);
    }
}

/// Profiling: µs spent in face sub-stages (corp+eyes, misreg ink, ink, scar+eyewear, decorations).
pub static PROF: [std::sync::atomic::AtomicU32; 5] = [const { std::sync::atomic::AtomicU32::new(0) }; 5];

fn c_has_shade(c: &Canvas) -> bool {
    c.has_shade()
}

/// Soot ground with halftone corners (the design's `bgLayer`).
fn background(c: &mut Canvas, pal: &Palette) {
    c.clear(pal.bg);
    let ad = pal.ad;
    let dots = move |x: usize, y: usize| {
        let dx = (x as f32 + 0.5).rem_euclid(6.0) - 3.0;
        let dy = (y as f32 + 0.5).rem_euclid(6.0) - 3.0;
        let cov = (1.8 - (dx * dx + dy * dy).sqrt()).clamp(0.0, 1.0);
        (cov > 0.0).then_some((ad, cov * 0.5))
    };
    c.fill_with(&Path::polygon(&[(200., 40.), (320., 40.), (320., 160.)]), dots);
    c.fill_with(&Path::polygon(&[(0., 120.), (0., 240.), (120., 240.)]), dots);
}

/// Vignette (the design's `overlay`, minus scanlines) as a keep factor per pixel.
fn overlay_shade() -> Vec<u8> {
    let mut m = vec![255u8; W * H];
    for y in 0..H {
        for x in 0..W {
            let dx = (x as f32 + 0.5 - 160.0) / 208.0;
            let dy = (y as f32 + 0.5 - 115.2) / 156.0;
            let d = (dx * dx + dy * dy).sqrt();
            // Vignette only toward the corners. No scanlines: at ~200 ppi they
            // don't read as a CRT, they just cut every third row of each glyph.
            let vig = ((d - 0.8) / 0.4).clamp(0.0, 1.0) * 0.45;
            let keep = 1.0 - vig;
            m[y * W + x] = (keep * 255.0 + 0.5) as u8;
        }
    }
    m
}

fn eye_path(cx: f32, cy: f32, hw: f32, top: f32, bot: f32) -> Path {
    Path::new()
        .move_to(cx - hw, cy)
        .quad_to(cx, cy - 2.0 * top, cx + hw, cy)
        .quad_to(cx, cy + 2.0 * bot, cx - hw, cy)
        .close()
}

/// Approximation of the design's `feGaussianBlur(4)` glow under the iris.
fn glow_ellipse(c: &mut Canvas, cx: f32, cy: f32, rx: f32, ry: f32, col: Rgb) {
    let xf = c.xf;
    let alpha = c.alpha;
    let (dcx, dcy) = xf.apply(cx, cy);
    let (drx, dry) = (rx * xf.s, ry * xf.s);
    let sigma = 4.0 * xf.s;
    let pad = 10.0;
    c.fill_with(&Path::ellipse(cx, cy, rx + pad, ry + pad), |x, y| {
        let dx = (x as f32 + 0.5 - dcx) / drx;
        let dy = (y as f32 + 0.5 - dcy) / dry;
        let d = (dx * dx + dy * dy).sqrt();
        let dist = (d - 1.0) * drx.min(dry);
        let z = 1.6 * dist / sigma;
        // Logistic falloff, cheap rational approximation of 1/(1+e^z).
        let a = 0.5 - 0.5 * z / (1.0 + z.abs() * 0.55);
        let a = a.clamp(0.0, 1.0);
        Some((col, a * alpha))
    });
}

/// The face's line art in one colour (the design's `ink()`).
/// `mode`: 0 open, 1 blink, 2 asleep.
#[allow(clippy::too_many_arguments)]
fn ink_layer(c: &mut Canvas, p: &Params, col: Rgb, panel: Rgb, lx: f32, rx: f32, cy: f32, mode: u8) {
    let hw = 34.0;
    for (ecx, top, bot, side) in [(lx, p.l_t, p.l_b, -1.0f32), (rx, p.r_t, p.r_b, 1.0)] {
        let fx2 = |x: f32| if side < 0.0 { x } else { 2.0 * ecx - x };
        match mode {
            0 => {
                let lid = Path::new().move_to(ecx - hw, cy).quad_to(ecx, cy - 2.0 * top, ecx + hw, cy);
                c.stroke_square(&lid, 5.0, col, 1.0);
                let lower = Path::new().move_to(ecx - hw + 4.0, cy + 1.0).quad_to(ecx, cy + 2.0 * bot, ecx + hw - 4.0, cy + 1.0);
                c.stroke(&lower, 1.5, col, 1.0);
                let o = ecx - hw;
                c.fill(&Path::polygon(&[(fx2(o + 4.0), cy - 2.0), (fx2(o - 14.0), cy - 8.0), (fx2(o + 12.0), cy - 7.0)]), col, 1.0);
            }
            2 => c.line(ecx - hw, cy + 2.0, ecx + hw, cy + 2.0, 4.0, col, 1.0),
            _ => {
                let shut = Path::new().move_to(ecx - hw, cy).quad_to(ecx, cy + 4.0, ecx + hw, cy);
                c.stroke(&shut, 4.0, col, 1.0);
            }
        }
        let by = cy + 2.0 * bot.max(4.0);
        let bag = Path::new().move_to(ecx - 18.0, by + 5.0).quad_to(ecx, by + 9.0, ecx + 14.0, by + 4.0);
        c.stroke(&bag, 1.2, col, 0.45);
    }
    let brow = |xi: f32, yi: f32, xo: f32, yo: f32| {
        let (mx, my) = ((xi + xo) / 2.0, (yi + yo) / 2.0);
        Path::polygon(&[(xi, yi - 3.0), (mx, my - 5.0), (xo, yo), (mx, my + 1.0), (xi, yi + 4.0)])
    };
    c.fill(&brow(lx + 24.0, cy + p.bli, lx - 34.0, cy + p.blo), col, 1.0);
    c.fill(&brow(rx - 24.0, cy + p.bri, rx + 34.0, cy + p.bro), col, 1.0);

    let (ox, oy) = (p.gx, p.gy);
    let mx = 160.0 + ox * 3.0;
    let my = 176.0 + oy * 2.0;
    let l = (mx - p.mw, my - p.sm * 0.3);
    let r = (mx + p.mw, my - p.sm * 0.3 - p.skew);
    if p.mo > 0.8 {
        let mouth = Path::new()
            .move_to(l.0, l.1)
            .quad_to(mx, my + p.sm, r.0, r.1)
            .quad_to(mx, my + p.sm + p.mo * 2.4, l.0, l.1)
            .close();
        c.fill(&mouth, panel, 1.0);
        c.stroke(&mouth, 2.5, col, 1.0);
        if p.fang > 0.5 {
            for u in [0.25f32, 0.75] {
                let x = l.0 + (r.0 - l.0) * u;
                let y = l.1 + (r.1 - l.1) * u + p.sm * 0.5 * (1.0 - (2.0 * u - 1.0).abs());
                c.fill(&Path::polygon(&[(x - 3.0, y), (x + 3.0, y), (x, y + 6.0)]), col, 1.0);
            }
        }
    } else {
        let line = Path::new().move_to(l.0, l.1).quad_to(mx, my + p.sm, r.0, r.1);
        c.stroke_square(&line, 3.5, col, 1.0);
    }
}
