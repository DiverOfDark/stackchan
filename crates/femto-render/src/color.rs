//! OKLCH palette (the design's colours) converted to sRGB.

use femto_core::settings::Accent;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rgb(pub u8, pub u8, pub u8);

impl Rgb {
    pub const BLACK: Rgb = Rgb(0, 0, 0);

    pub fn to_565(self) -> u16 {
        ((self.0 as u16 & 0xF8) << 8) | ((self.1 as u16 & 0xFC) << 3) | (self.2 as u16 >> 3)
    }
}

/// OKLCH (L 0..1, C, hue degrees) → sRGB, clamped to gamut.
pub fn oklch(l: f32, c: f32, h_deg: f32) -> Rgb {
    let h = h_deg.to_radians();
    let (a, b) = (c * h.cos(), c * h.sin());
    let l_ = l + 0.396_337_78 * a + 0.215_803_76 * b;
    let m_ = l - 0.105_561_346 * a - 0.063_854_17 * b;
    let s_ = l - 0.089_484_18 * a - 1.291_485_5 * b;
    let (l3, m3, s3) = (l_ * l_ * l_, m_ * m_ * m_, s_ * s_ * s_);
    let r = 4.076_741_7 * l3 - 3.307_711_6 * m3 + 0.230_969_94 * s3;
    let g = -1.268_438 * l3 + 2.609_757_4 * m3 - 0.341_319_38 * s3;
    let bl = -0.004_196_086_3 * l3 - 0.703_418_6 * m3 + 1.707_614_7 * s3;
    let enc = |x: f32| {
        let x = x.clamp(0.0, 1.0);
        let v = if x <= 0.003_130_8 { 12.92 * x } else { 1.055 * x.powf(1.0 / 2.4) - 0.055 };
        (v * 255.0 + 0.5) as u8
    };
    Rgb(enc(r), enc(g), enc(bl))
}

/// Every colour a screen uses.
#[derive(Clone, Copy, Debug)]
pub struct Palette {
    /// Bone: line art and text.
    pub ink: Rgb,
    /// Soot: screen ground.
    pub bg: Rgb,
    pub panel: Rgb,
    /// Dead eye whites.
    pub sclera: Rgb,
    pub sec: Rgb,
    pub dim: Rgb,
    /// Caution 60–84 %.
    pub toxic: Rgb,
    /// Accent, accent-dark, accent-light.
    pub a: Rgb,
    pub ad: Rgb,
    pub al: Rgb,
}

impl Palette {
    pub fn new(accent: Accent) -> Palette {
        let (a, ad, al) = match accent {
            Accent::SignalRed => (oklch(0.6, 0.23, 25.), oklch(0.38, 0.15, 25.), oklch(0.75, 0.17, 40.)),
            Accent::ToxicGreen => (oklch(0.75, 0.2, 140.), oklch(0.42, 0.13, 145.), oklch(0.88, 0.15, 125.)),
            Accent::IceBlue => (oklch(0.72, 0.14, 225.), oklch(0.42, 0.1, 235.), oklch(0.86, 0.09, 210.)),
        };
        Palette {
            ink: oklch(0.88, 0.02, 80.),
            bg: oklch(0.12, 0.015, 20.),
            panel: oklch(0.07, 0.01, 20.),
            sclera: oklch(0.2, 0.02, 20.),
            // Brighter than the design's 0.72: labels must read on the panel.
            sec: oklch(0.8, 0.02, 60.),
            dim: oklch(0.3, 0.02, 20.),
            toxic: oklch(0.86, 0.17, 110.),
            a,
            ad,
            al,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn white_and_black() {
        assert_eq!(oklch(1.0, 0.0, 0.0), Rgb(255, 255, 255));
        assert_eq!(oklch(0.0, 0.0, 0.0), Rgb(0, 0, 0));
    }

    #[test]
    fn signal_red_is_red() {
        let Rgb(r, g, b) = oklch(0.6, 0.23, 25.);
        assert!(r > 200 && g < 80 && b < 90, "{r} {g} {b}");
    }
}
