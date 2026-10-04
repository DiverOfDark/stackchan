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
    /// Terminus bitmap 6×12 regular / bold, 8×14 bold, 8×16 bold: crisp
    /// small text (system readouts) on the 2" panel. Size is fixed.
    Term12,
    Term12B,
    Term14B,
    Term16B,
}

impl Face {
    fn bitmap(self) -> Option<&'static [u8]> {
        Some(match self {
            Face::Term12 => include_bytes!("../../../assets/fonts/terminus-12n.fbf"),
            Face::Term12B => include_bytes!("../../../assets/fonts/terminus-12b.fbf"),
            Face::Term14B => include_bytes!("../../../assets/fonts/terminus-14b.fbf"),
            Face::Term16B => include_bytes!("../../../assets/fonts/terminus-16b.fbf"),
            _ => return None,
        })
    }
}

/// A parsed `.fbf` monospace bitmap font (see tools/bdf2bin.py).
struct BitmapFont {
    w: usize,
    h: usize,
    ascent: i32,
    glyphs: HashMap<char, Vec<u8>>,
}

impl BitmapFont {
    fn parse(d: &[u8]) -> BitmapFont {
        assert_eq!(&d[..4], b"FBF1", "bitmap font header");
        let (w, h, ascent) = (d[4] as usize, d[5] as usize, d[6] as i32);
        let count = u16::from_le_bytes([d[7], d[8]]) as usize;
        let bpr = w.div_ceil(8);
        let mut glyphs = HashMap::with_capacity(count);
        let mut i = 9;
        for _ in 0..count {
            let cp = u32::from_le_bytes([d[i], d[i + 1], d[i + 2], d[i + 3]]);
            i += 4;
            let mut cov = vec![0u8; w * h];
            for y in 0..h {
                for x in 0..w {
                    if d[i + y * bpr + x / 8] & (0x80 >> (x % 8)) != 0 {
                        cov[y * w + x] = 255;
                    }
                }
            }
            i += bpr * h;
            if let Some(c) = char::from_u32(cp) {
                glyphs.insert(c, cov);
            }
        }
        BitmapFont { w, h, ascent, glyphs }
    }
}

/// Steepen anti-aliasing ramps: unhinted outlines at 10–24 px smear over
/// two pixels; this firms the edge without going jagged.
fn crisp(cov: &mut [u8]) {
    for c in cov {
        let v = *c as f32 / 255.0;
        let v = ((v - 0.18) / 0.64).clamp(0.0, 1.0);
        *c = (v * 255.0 + 0.5) as u8;
    }
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

/// Cyrillic for the Barlow faces, which have none (Google's Barlow is Latin
/// only): Fira Sans Condensed at the matching weight, subset to Cyrillic.
/// Captions carry Russian speech.
const CYRILLIC_DATA: [(Face, &[u8]); 4] = [
    (Face::Medium, include_bytes!("../../../assets/fonts/FiraSansCondensed-Medium-cyrillic.ttf")),
    (Face::SemiBold, include_bytes!("../../../assets/fonts/FiraSansCondensed-SemiBold-cyrillic.ttf")),
    (Face::BoldItalic, include_bytes!("../../../assets/fonts/FiraSansCondensed-BoldItalic-cyrillic.ttf")),
    (Face::BlackItalic, include_bytes!("../../../assets/fonts/FiraSansCondensed-ExtraBoldItalic-cyrillic.ttf")),
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
    /// Per-face fallback for glyphs the face lacks (see CYRILLIC_DATA).
    cyrillic: HashMap<Face, Font>,
    bitmaps: HashMap<Face, BitmapFont>,
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
        let cyrillic = CYRILLIC_DATA
            .iter()
            .map(|&(face, data)| (face, Font::from_bytes(data, FontSettings::default()).expect("bundled font parses")))
            .collect();
        let bitmaps = [Face::Term12, Face::Term12B, Face::Term14B, Face::Term16B]
            .into_iter()
            .map(|f| (f, BitmapFont::parse(f.bitmap().unwrap())))
            .collect();
        Fonts { fonts, cyrillic, bitmaps, cache: HashMap::new() }
    }

    fn glyph(&mut self, face: Face, size: f32, ch: char) -> &Glyph {
        if let Some(bf) = self.bitmaps.get(&face) {
            // Native size only; unknown chars render as '?'.
            let key = (face, 0, ch);
            return self.cache.entry(key).or_insert_with(|| {
                let cov = bf.glyphs.get(&ch).or_else(|| bf.glyphs.get(&'?')).cloned().unwrap_or_else(|| vec![0; bf.w * bf.h]);
                Glyph { xmin: 0, ymin: bf.ascent - bf.h as i32, w: bf.w, h: bf.h, advance: bf.w as f32, cov }
            });
        }
        let key = (face, (size * 4.0).round() as u32, ch);
        if !self.cache.contains_key(&key) {
            let (m, mut cov) = self.font_for(face, ch).rasterize(ch, size);
            if size <= 24.0 {
                crisp(&mut cov);
            }
            let g = Glyph { xmin: m.xmin, ymin: m.ymin, w: m.width, h: m.height, advance: m.advance_width, cov };
            self.cache.insert(key, g);
        }
        &self.cache[&key]
    }

    /// The font that draws `ch` in `face`: the face itself, else its
    /// Cyrillic companion (Mono borrows Medium's), else SemiBold.
    fn font_for(&self, face: Face, ch: char) -> &Font {
        let own = &self.fonts[&face];
        if own.has_glyph(ch) {
            return own;
        }
        let companion = if face == Face::Mono { Face::Medium } else { face };
        match self.cyrillic.get(&companion) {
            Some(f) if f.has_glyph(ch) => f,
            _ => &self.fonts[&Face::SemiBold],
        }
    }

    /// Cached glyph count (diagnostics).
    pub fn cached(&self) -> usize {
        self.cache.len()
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_outline_face_draws_cyrillic() {
        let fonts = Fonts::new();
        for face in [Face::Medium, Face::SemiBold, Face::BoldItalic, Face::BlackItalic, Face::Mono] {
            for ch in "Эй, Фемто! ЁЖЩЪЫЬЮЯ ёжщъыьюя «»—№".chars().filter(|c| !c.is_ascii()) {
                assert!(fonts.font_for(face, ch).has_glyph(ch), "{face:?} lacks {ch}");
            }
        }
    }

    #[test]
    fn latin_stays_in_barlow() {
        let fonts = Fonts::new();
        assert!(std::ptr::eq(fonts.font_for(Face::Medium, 'A'), &fonts.fonts[&Face::Medium]));
    }
}
