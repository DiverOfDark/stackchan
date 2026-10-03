//! Text: the design's four faces, rasterized on demand by fontdue and cached.

use std::collections::HashMap;

use fontdue::{Font, FontSettings};

use crate::canvas::Canvas;
use crate::color::Rgb;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Face {
    /// Barlow Condensed 500 (captions).
    Medium,
    /// Barlow Condensed 600 (default speech/labels).
    SemiBold,
    /// Barlow Condensed 700 italic (tags).
    BoldItalic,
    /// Barlow Condensed 800 italic (numbers, headings).
    BlackItalic,
    /// Share Tech Mono (system readouts).
    Mono,
    /// Noto Sans JP 900 subset (フェムト警告監視).
    Jp,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Anchor {
    Start,
    Middle,
    End,
}

const FONT_DATA: [(Face, &[u8]); 6] = [
    (Face::Medium, include_bytes!("../../../assets/fonts/BarlowCondensed-Medium.ttf")),
    (Face::SemiBold, include_bytes!("../../../assets/fonts/BarlowCondensed-SemiBold.ttf")),
    (Face::BoldItalic, include_bytes!("../../../assets/fonts/BarlowCondensed-BoldItalic.ttf")),
    (Face::BlackItalic, include_bytes!("../../../assets/fonts/BarlowCondensed-ExtraBoldItalic.ttf")),
    (Face::Mono, include_bytes!("../../../assets/fonts/ShareTechMono-Regular.ttf")),
    (Face::Jp, include_bytes!("../../../assets/fonts/NotoSansJP-Black-subset.ttf")),
];

struct Glyph {
    xmin: i32,
    ymin: i32,
    w: usize,
    h: usize,
    advance: f32,
    cov: Vec<u8>,
}

pub struct Fonts {
    fonts: HashMap<Face, Font>,
    /// Keyed by face, size in quarter-pixels, char.
    cache: HashMap<(Face, u32, char), Glyph>,
}

impl Default for Fonts {
    fn default() -> Self {
        Fonts::new()
    }
}

impl Fonts {
    pub fn new() -> Fonts {
        let fonts = FONT_DATA
            .iter()
            .map(|&(face, data)| (face, Font::from_bytes(data, FontSettings::default()).expect("bundled font parses")))
            .collect();
        Fonts { fonts, cache: HashMap::new() }
    }

    fn glyph(&mut self, face: Face, size: f32, ch: char) -> &Glyph {
        let key = (face, (size * 4.0).round() as u32, ch);
        let fonts = &self.fonts;
        self.cache.entry(key).or_insert_with(|| {
            let font = &fonts[&face];
            // Kana/kanji only exist in the JP subset; everything else falls
            // back to SemiBold when a face lacks the glyph.
            let font = if font.has_glyph(ch) { font } else { &fonts[&Face::SemiBold] };
            let (m, cov) = font.rasterize(ch, size);
            Glyph { xmin: m.xmin, ymin: m.ymin, w: m.width, h: m.height, advance: m.advance_width, cov }
        })
    }

    pub fn measure(&mut self, face: Face, size: f32, s: &str, letter_spacing: f32) -> f32 {
        s.chars().map(|c| self.glyph(face, size, c).advance + letter_spacing).sum()
    }
}

/// Text style; mirrors the design's `T()` options.
#[derive(Clone, Copy, Debug)]
pub struct Style {
    pub face: Face,
    pub size: f32,
    pub color: Rgb,
    pub anchor: Anchor,
    pub letter_spacing: f32,
    pub opacity: f32,
}

impl Style {
    pub fn new(face: Face, size: f32, color: Rgb) -> Style {
        Style { face, size, color, anchor: Anchor::Start, letter_spacing: 0.0, opacity: 1.0 }
    }
    pub fn middle(mut self) -> Style {
        self.anchor = Anchor::Middle;
        self
    }
    pub fn end(mut self) -> Style {
        self.anchor = Anchor::End;
        self
    }
    pub fn spacing(mut self, ls: f32) -> Style {
        self.letter_spacing = ls;
        self
    }
    pub fn opacity(mut self, o: f32) -> Style {
        self.opacity = o;
        self
    }
}

impl Canvas {
    /// Draw `s` with its baseline at (`x`, `y`) in user space.
    pub fn text(&mut self, fonts: &mut Fonts, x: f32, y: f32, s: &str, st: Style) {
        let scale = self.xf.s;
        let size = st.size * scale;
        let ls = st.letter_spacing * scale;
        let (mut px, py) = self.xf.apply(x, y);
        let width = fonts.measure(st.face, size, s, ls);
        match st.anchor {
            Anchor::Start => {}
            Anchor::Middle => px -= width / 2.0,
            Anchor::End => px -= width,
        }
        for ch in s.chars() {
            let g = fonts.glyph(st.face, size, ch);
            let gx = (px + g.xmin as f32).round() as i32;
            let gy = (py - g.ymin as f32 - g.h as f32).round() as i32;
            self.blit_alpha(gx, gy, g.w, g.h, &g.cov, st.color, st.opacity);
            let advance = g.advance;
            px += advance + ls;
        }
    }
}
