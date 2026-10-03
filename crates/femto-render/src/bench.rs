//! Micro-benchmarks of rasterizer primitives, run on the device at boot.

use std::time::Instant;

use crate::canvas::Canvas;
use crate::color::Rgb;
use crate::path::Path;
use crate::text::{Face, Fonts, Style};

pub fn run(c: &mut Canvas, fonts: &mut Fonts) -> Vec<(&'static str, u32)> {
    let red = Rgb(200, 30, 30);
    let mut out = Vec::new();
    let mut time = |name: &'static str, c: &mut Canvas, f: &mut dyn FnMut(&mut Canvas)| {
        f(c);
        let t = Instant::now();
        for _ in 0..10 {
            f(c);
        }
        out.push((name, (t.elapsed().as_micros() / 10) as u32));
    };
    time("rect 100x100 opaque", c, &mut |c| c.fill(&Path::rect(10., 10., 100., 100.), red, 1.0));
    time("rect 100x100 alpha", c, &mut |c| c.fill(&Path::rect(10., 10., 100., 100.), red, 0.5));
    time("path build ellipse r30", c, &mut |_| {
        std::hint::black_box(Path::ellipse(100., 100., 30., 30.));
    });
    time("ellipse r30 fill", c, &mut |c| c.fill(&Path::ellipse(100., 100., 30., 30.), red, 1.0));
    time("quad stroke w5", c, &mut |c| c.stroke(&Path::new().move_to(60., 100.).quad_to(100., 70., 140., 100.), 5., red, 1.0));
    time("line w2 200px", c, &mut |c| c.line(10., 10., 210., 60., 2., red, 1.0));
    time("fill_with shader 60x60", c, &mut |c| {
        c.fill_with(&Path::rect(10., 10., 60., 60.), |x, y| Some((red, ((x + y) % 7) as f32 / 7.0)))
    });
    time("clip + ellipse", c, &mut |c| {
        let eye = Path::ellipse(100., 100., 34., 16.);
        c.with_clip(&eye, |c| c.fill(&Path::ellipse(100., 100., 14., 17.), red, 1.0));
    });
    time("text 20 chars mono 10", c, &mut |c| c.text(fonts, 10., 100., "RST THU 09:00 SESSIO", Style::new(Face::Mono, 10., red)));
    time("text 3 chars heavy 40", c, &mut |c| c.text(fonts, 10., 100., "38%", Style::new(Face::BlackItalic, 40., red)));
    time("f32 sqrt x10000", c, &mut |_| {
        let mut s = 0.0f32;
        for i in 0..10_000 {
            s += std::hint::black_box(i as f32).sqrt();
        }
        std::hint::black_box(s);
    });
    time("f32 div x10000", c, &mut |_| {
        let mut s = 0.0f32;
        for i in 1..10_001 {
            s += 1.0 / std::hint::black_box(i as f32);
        }
        std::hint::black_box(s);
    });
    time("f32 floor x10000", c, &mut |_| {
        let mut s = 0.0f32;
        for i in 0..10_000 {
            s += std::hint::black_box(i as f32 * 0.37).floor();
        }
        std::hint::black_box(s);
    });
    out
}
