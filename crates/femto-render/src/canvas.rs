//! RGB565 canvas with an anti-aliased scanline rasterizer (signed-area
//! accumulation, as in font-rs), a uniform-scale transform, a clip mask and
//! a group opacity.

use crate::color::Rgb;
use crate::path::Path;

pub const W: usize = 320;
pub const H: usize = 240;

#[derive(Clone, Copy, Debug)]
pub struct Xf {
    pub s: f32,
    pub tx: f32,
    pub ty: f32,
}

impl Xf {
    pub const ID: Xf = Xf { s: 1.0, tx: 0.0, ty: 0.0 };

    pub fn apply(&self, x: f32, y: f32) -> (f32, f32) {
        (x * self.s + self.tx, y * self.s + self.ty)
    }

    /// `self` then `inner` (inner coordinates are mapped by inner first).
    pub fn then(&self, inner: Xf) -> Xf {
        Xf { s: self.s * inner.s, tx: self.tx + inner.tx * self.s, ty: self.ty + inner.ty * self.s }
    }
}

/// Coverage mask over a device-space box.
struct Clip {
    x0: usize,
    y0: usize,
    w: usize,
    h: usize,
    mask: Vec<u8>,
}

impl Clip {
    #[inline]
    fn at(&self, x: usize, y: usize) -> u32 {
        if x < self.x0 || y < self.y0 || x >= self.x0 + self.w || y >= self.y0 + self.h {
            return 0;
        }
        self.mask[(y - self.y0) * self.w + (x - self.x0)] as u32
    }
}

/// A window of full-width rows `y0..y0 + rows` of the 320×240 screen.
/// Drawing uses screen coordinates; anything outside the window is skipped.
/// The full-screen canvas is the window `0..240`.
pub struct Canvas {
    px: Vec<u16>,
    y0: usize,
    rows: usize,
    clip: Option<Clip>,
    /// Post-shade applied at scan-out: per-pixel keep factor (255 = as is).
    shade: Option<Vec<u8>>,
    acc: Vec<f32>,
    pub xf: Xf,
    /// Group opacity multiplier for everything drawn.
    pub alpha: f32,
}

impl Default for Canvas {
    fn default() -> Self {
        Canvas::new()
    }
}

impl Canvas {
    pub fn new() -> Canvas {
        Canvas { px: vec![0; W * H], y0: 0, rows: H, clip: None, shade: None, acc: Vec::with_capacity((W + 2) * H), xf: Xf::ID, alpha: 1.0 }
    }

    /// A strip canvas `rows` tall; move it with [`Canvas::set_window`].
    pub fn strip(rows: usize) -> Canvas {
        Canvas::with_buffer(vec![0; W * rows])
    }

    /// Point the strip at screen rows `y0..y0 + rows`.
    pub fn set_window(&mut self, y0: usize) {
        assert!(y0 + self.rows <= H);
        self.y0 = y0;
    }

    /// (first row, row count).
    pub fn window(&self) -> (usize, usize) {
        (self.y0, self.rows)
    }

    /// Use `scratch` (its capacity, in f32s) for rasterization. On the device
    /// this is a small internal-RAM buffer; taller shapes are done in bands.
    pub fn set_scratch(&mut self, scratch: Vec<f32>) {
        assert!(scratch.capacity() >= W + 2, "scratch must hold at least one row");
        self.acc = scratch;
    }

    /// Canvas over a caller-provided buffer of whole rows.
    pub fn with_buffer(px: Vec<u16>) -> Canvas {
        assert!(px.len() % W == 0 && px.len() <= W * H && !px.is_empty());
        let rows = px.len() / W;
        Canvas { px, y0: 0, rows, clip: None, shade: None, acc: Vec::with_capacity((W + 2) * 16), xf: Xf::ID, alpha: 1.0 }
    }

    pub fn pixels(&self) -> &[u16] {
        &self.px
    }

    pub fn pixels_mut(&mut self) -> &mut [u16] {
        &mut self.px
    }

    pub fn clear(&mut self, c: Rgb) {
        self.px.fill(c.to_565());
    }

    /// Set the scan-out shade (vignette + scanlines), or `None`.
    pub fn set_shade(&mut self, shade: Option<Vec<u8>>) {
        self.shade = shade;
    }

    pub fn has_shade(&self) -> bool {
        self.shade.is_some()
    }

    /// Final pixel `i` of the window with the shade applied.
    #[inline]
    pub fn out_px(&self, i: usize) -> u16 {
        match &self.shade {
            Some(s) => scale565(self.px[i], s[self.y0 * W + i] as u32),
            None => self.px[i],
        }
    }

    /// Final window pixels `start..start + out.len()`, byte-swapped for SPI if asked.
    pub fn scanout(&self, start: usize, out: &mut [u16], swap: bool) {
        let src = &self.px[start..start + out.len()];
        match &self.shade {
            Some(s) => {
                let base = self.y0 * W + start;
                let s = &s[base..base + out.len()];
                for ((d, &p), &k) in out.iter_mut().zip(src).zip(s) {
                    let v = scale565(p, k as u32);
                    *d = if swap { v.swap_bytes() } else { v };
                }
            }
            None => {
                for (d, &p) in out.iter_mut().zip(src) {
                    *d = if swap { p.swap_bytes() } else { p };
                }
            }
        }
    }

    /// RGB888 copy (shade applied), for PNG export and the simulator.
    pub fn to_rgb888(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(self.px.len() * 3);
        for i in 0..self.px.len() {
            let (r, g, b) = unpack(self.out_px(i));
            out.extend_from_slice(&[r, g, b]);
        }
        out
    }

    // ---- state ------------------------------------------------------------

    pub fn with_xf<R>(&mut self, xf: Xf, f: impl FnOnce(&mut Canvas) -> R) -> R {
        let saved = self.xf;
        self.xf = saved.then(xf);
        let r = f(self);
        self.xf = saved;
        r
    }

    pub fn with_alpha<R>(&mut self, a: f32, f: impl FnOnce(&mut Canvas) -> R) -> R {
        let saved = self.alpha;
        self.alpha *= a;
        let r = f(self);
        self.alpha = saved;
        r
    }

    /// Draw `f` clipped to `path`.
    pub fn with_clip<R>(&mut self, path: &Path, f: impl FnOnce(&mut Canvas) -> R) -> R {
        let mut pts: Vec<(u16, u16, u8)> = Vec::new();
        let win = (self.y0, self.y0 + self.rows);
        rasterize(&mut self.acc, self.xf, win, path, |x, y, cov| pts.push((x as u16, y as u16, (cov * 255.0 + 0.5) as u8)));
        let (mut x0, mut y0, mut x1, mut y1) = (W, H, 0, 0);
        for &(x, y, _) in &pts {
            x0 = x0.min(x as usize);
            y0 = y0.min(y as usize);
            x1 = x1.max(x as usize + 1);
            y1 = y1.max(y as usize + 1);
        }
        let (w, h) = (x1.saturating_sub(x0), y1.saturating_sub(y0));
        let mut mask = vec![0u8; w * h];
        for (x, y, c) in pts {
            mask[(y as usize - y0) * w + (x as usize - x0)] = c;
        }
        let saved = self.clip.replace(Clip { x0, y0, w, h, mask });
        let r = f(self);
        self.clip = saved;
        r
    }

    // ---- drawing ----------------------------------------------------------

    pub fn fill(&mut self, path: &Path, c: Rgb, opacity: f32) {
        let a = opacity * self.alpha;
        if a <= 0.0 {
            return;
        }
        let c = Rgb(c.0, c.1, c.2);
        self.fill_with(path, |_, _| Some((c, a)));
    }

    pub fn stroke(&mut self, path: &Path, width: f32, c: Rgb, opacity: f32) {
        self.fill(&path.stroke(width, false), c, opacity);
    }

    pub fn stroke_square(&mut self, path: &Path, width: f32, c: Rgb, opacity: f32) {
        self.fill(&path.stroke(width, true), c, opacity);
    }

    pub fn line(&mut self, x1: f32, y1: f32, x2: f32, y2: f32, width: f32, c: Rgb, opacity: f32) {
        self.stroke(&Path::line(x1, y1, x2, y2), width, c, opacity);
    }

    /// Fill with a per-pixel shader; `shade(x, y)` gets device pixel
    /// coordinates and returns colour and opacity (already including any
    /// group alpha the caller wants).
    pub fn fill_with(&mut self, path: &Path, shade: impl Fn(usize, usize) -> Option<(Rgb, f32)>) {
        let mut acc = std::mem::take(&mut self.acc);
        let win = (self.y0, self.y0 + self.rows);
        rasterize(&mut acc, self.xf, win, path, |x, y, cov| {
            if let Some((c, a)) = shade(x, y) {
                self.blend(x, y, c, a * cov);
            }
        });
        self.acc = acc;
    }

    /// Per-pixel effect over a device-space rectangle.
    pub fn effect(&mut self, x0: usize, y0: usize, x1: usize, y1: usize, f: impl Fn(usize, usize) -> Option<(Rgb, f32)>) {
        for y in y0.max(self.y0)..y1.min(self.y0 + self.rows) {
            for x in x0..x1.min(W) {
                if let Some((c, a)) = f(x, y) {
                    self.blend(x, y, c, a);
                }
            }
        }
    }

    /// Blend an 8-bit coverage bitmap at device position (`x`, `y`).
    pub fn blit_alpha(&mut self, x: i32, y: i32, w: usize, h: usize, cov: &[u8], c: Rgb, opacity: f32) {
        let a = opacity * self.alpha;
        for row in 0..h {
            let py = y + row as i32;
            if !(self.y0 as i32..(self.y0 + self.rows) as i32).contains(&py) {
                continue;
            }
            for col in 0..w {
                let px = x + col as i32;
                if !(0..W as i32).contains(&px) {
                    continue;
                }
                let v = cov[row * w + col];
                if v > 0 {
                    self.blend(px as usize, py as usize, c, a * v as f32 * (1.0 / 255.0));
                }
            }
        }
    }

    #[inline]
    fn blend(&mut self, x: usize, y: usize, c: Rgb, a: f32) {
        let idx = (y - self.y0) * W + x;
        // Fixed-point alpha 0..=256.
        let mut a = (a * 256.0) as u32;
        if let Some(m) = &self.clip {
            a = (a * m.at(x, y)) / 255;
        }
        if a == 0 {
            return;
        }
        let src = c.to_565();
        if a >= 255 {
            self.px[idx] = src;
            return;
        }
        self.px[idx] = mix565(self.px[idx], src, a);
    }

}

/// Diagnostics: fills, bbox pixels scanned.
pub static STATS: [core::sync::atomic::AtomicU32; 2] = [const { core::sync::atomic::AtomicU32::new(0) }; 2];

/// Scan-convert `path` (transformed by `xf`) within screen rows `win` and
/// call `emit(x, y, coverage)` for every covered pixel. `acc` is scratch.
fn rasterize(acc: &mut Vec<f32>, xf: Xf, win: (usize, usize), path: &Path, mut emit: impl FnMut(usize, usize, f32)) {
    {
        let mut minx = f32::MAX;
        let mut miny = f32::MAX;
        let mut maxx = f32::MIN;
        let mut maxy = f32::MIN;
        let contours: Vec<Vec<(f32, f32)>> = path
            .contours
            .iter()
            .filter(|c| c.pts.len() > 1)
            .map(|c| {
                c.pts
                    .iter()
                    .map(|p| {
                        let (x, y) = xf.apply(p.x, p.y);
                        minx = minx.min(x);
                        miny = miny.min(y);
                        maxx = maxx.max(x);
                        maxy = maxy.max(y);
                        (x, y)
                    })
                    .collect()
            })
            .collect();
        if contours.is_empty() {
            return;
        }
        let bx0 = (minx.floor().max(0.0)) as usize;
        let by0 = (miny.floor().max(win.0 as f32)) as usize;
        let bx1 = (maxx.ceil().min(W as f32)).max(0.0) as usize;
        let by1 = (maxy.ceil().min(win.1 as f32)).max(0.0) as usize;
        if bx1 <= bx0 || by1 <= by0 {
            return;
        }
        let bw = bx1 - bx0;
        let bh = by1 - by0;
        STATS[0].fetch_add(1, core::sync::atomic::Ordering::Relaxed);
        STATS[1].fetch_add((bw * bh) as u32, core::sync::atomic::Ordering::Relaxed);
        let stride = bw + 2;
        let band = (acc.capacity() / stride).clamp(1, bh);
        let (ox, oy) = (bx0 as f32, by0 as f32);
        // Per-row span of columns any edge touched: outside it the running
        // sum is zero (left) or unchanged and zero (right, closed paths).
        let mut spans = vec![(u16::MAX, 0u16); band];
        let mut row0 = 0;
        while row0 < bh {
            let rows = band.min(bh - row0);
            acc.clear();
            acc.resize(stride * rows, 0.0);
            spans[..rows].fill((u16::MAX, 0));
            let oy = oy + row0 as f32;
            for c in &contours {
                for i in 0..c.len() {
                    let a = c[i];
                    let b = c[(i + 1) % c.len()];
                    accumulate(acc, &mut spans, stride, bw, rows, (a.0 - ox, a.1 - oy), (b.0 - ox, b.1 - oy));
                }
            }
            for row in 0..rows {
                let (lo, hi) = spans[row];
                if lo > hi {
                    continue;
                }
                let mut sum = 0.0f32;
                let line = &acc[row * stride..row * stride + stride];
                let hi = (hi as usize + 1).min(bw);
                for (col, &v) in line.iter().enumerate().take(hi).skip(lo as usize) {
                    sum += v;
                    let cov = sum.abs().min(1.0);
                    if cov > 0.002 {
                        let (x, y) = (bx0 + col, by0 + row0 + row);
                        emit(x, y, cov);
                    }
                }
            }
            row0 += rows;
        }
    }
}

/// `d + (s − d)·a/256`, per 565 channel.
#[inline]
fn mix565(d: u16, s: u16, a: u32) -> u16 {
    let (d, s, a) = (d as i32, s as i32, a as i32);
    let ch = |shift: u32, mask: i32| {
        let dv = (d >> shift) & mask;
        let sv = (s >> shift) & mask;
        ((dv + (((sv - dv) * a) >> 8)) & mask) << shift
    };
    (ch(11, 0x1F) | ch(5, 0x3F) | ch(0, 0x1F)) as u16
}

/// Scale a 565 colour by `k`/255.
#[inline]
fn scale565(p: u16, k: u32) -> u16 {
    if k >= 255 {
        return p;
    }
    let p = p as u32;
    let r = ((p >> 11) & 0x1F) * k / 255;
    let g = ((p >> 5) & 0x3F) * k / 255;
    let b = (p & 0x1F) * k / 255;
    ((r << 11) | (g << 5) | b) as u16
}

pub fn unpack(p: u16) -> (u8, u8, u8) {
    let r = ((p >> 11) & 0x1F) as u8;
    let g = ((p >> 5) & 0x3F) as u8;
    let b = (p & 0x1F) as u8;
    ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
}

/// Signed-area accumulation of one edge into `acc` (row stride `stride`,
/// usable width `w`). X is clamped per row, which is exact for fills because
/// coverage left of the box is equivalent to coverage at column 0.
fn accumulate(acc: &mut [f32], spans: &mut [(u16, u16)], stride: usize, w: usize, h: usize, p0: (f32, f32), p1: (f32, f32)) {
    if (p0.1 - p1.1).abs() < 1e-6 {
        return;
    }
    let (dir, p0, p1) = if p0.1 < p1.1 { (1.0, p0, p1) } else { (-1.0, p1, p0) };
    let dxdy = (p1.0 - p0.0) / (p1.1 - p0.1);
    let wf = w as f32;
    let mut x = p0.0;
    if p0.1 < 0.0 {
        x -= p0.1 * dxdy;
    }
    let y_start = p0.1.max(0.0) as usize;
    let y_end = (p1.1.ceil().max(0.0) as usize).min(h);
    for y in y_start..y_end {
        let dy = ((y + 1) as f32).min(p1.1) - (y as f32).max(p0.1);
        let xnext = x + dxdy * dy;
        let d = dy * dir;
        let (xa, xb) = if x < xnext { (x, xnext) } else { (xnext, x) };
        let (xa, xb) = (xa.clamp(0.0, wf), xb.clamp(0.0, wf));
        let line = y * stride;
        let sp = &mut spans[y];
        sp.0 = sp.0.min(xa as u16);
        sp.1 = sp.1.max((xb as u16 + 2).min(w as u16));
        let x0f = xa.floor();
        let x0i = x0f as usize;
        let x1c = xb.ceil();
        let x1i = x1c as usize;
        if x1i <= x0i + 1 {
            let xm = 0.5 * (xa + xb) - x0f;
            acc[line + x0i] += d - d * xm;
            acc[line + x0i + 1] += d * xm;
        } else {
            let s = 1.0 / (xb - xa);
            let x0r = xa - x0f;
            let a0 = 0.5 * s * (1.0 - x0r) * (1.0 - x0r);
            let x1r = xb - x1c + 1.0;
            let am = 0.5 * s * x1r * x1r;
            acc[line + x0i] += d * a0;
            if x1i == x0i + 2 {
                acc[line + x0i + 1] += d * (1.0 - a0 - am);
            } else {
                let a1 = s * (1.5 - x0r);
                acc[line + x0i + 1] += d * (a1 - a0);
                for xi in x0i + 2..x1i - 1 {
                    acc[line + xi] += d * s;
                }
                let a2 = a1 + (x1i - x0i - 3) as f32 * s;
                acc[line + x1i - 1] += d * (1.0 - a2 - am);
            }
            acc[line + x1i] += d * am;
        }
        x = xnext;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rect_fill_exact() {
        let mut c = Canvas::new();
        c.fill(&Path::rect(10.0, 10.0, 5.0, 5.0), Rgb(255, 255, 255), 1.0);
        let px = c.pixels();
        assert_eq!(px[12 * W + 12], 0xFFFF);
        assert_eq!(px[12 * W + 15], 0);
        assert_eq!(px[9 * W + 12], 0);
    }

    #[test]
    fn half_pixel_edge_is_grey() {
        let mut c = Canvas::new();
        c.fill(&Path::rect(10.5, 10.0, 5.0, 5.0), Rgb(255, 255, 255), 1.0);
        let (r, _, _) = unpack(c.pixels()[12 * W + 10]);
        assert!((120..136).contains(&r), "{r}");
    }

    #[test]
    fn offscreen_shapes_do_not_panic() {
        let mut c = Canvas::new();
        c.fill(&Path::rect(-50.0, -50.0, 500.0, 400.0), Rgb(1, 2, 3), 1.0);
        c.line(300.0, 10.0, 340.0, 20.0, 3.0, Rgb(255, 0, 0), 1.0);
        c.fill(&Path::circle(-100.0, -100.0, 5.0), Rgb(255, 0, 0), 1.0);
    }

    #[test]
    fn clip_limits_fill() {
        let mut c = Canvas::new();
        c.with_clip(&Path::rect(0.0, 0.0, 10.0, 10.0), |c| {
            c.fill(&Path::rect(0.0, 0.0, 100.0, 100.0), Rgb(255, 255, 255), 1.0)
        });
        assert_eq!(c.pixels()[5 * W + 5], 0xFFFF);
        assert_eq!(c.pixels()[50 * W + 50], 0);
    }
}
