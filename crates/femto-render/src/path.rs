//! Vector paths: building, flattening, stroking, dashing. User-space only;
//! the canvas applies its transform when filling.

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Pt {
    pub x: f32,
    pub y: f32,
}

pub const fn pt(x: f32, y: f32) -> Pt {
    Pt { x, y }
}

impl Pt {
    fn sub(self, o: Pt) -> Pt {
        pt(self.x - o.x, self.y - o.y)
    }
    fn add(self, o: Pt) -> Pt {
        pt(self.x + o.x, self.y + o.y)
    }
    fn mul(self, k: f32) -> Pt {
        pt(self.x * k, self.y * k)
    }
    fn len(self) -> f32 {
        (self.x * self.x + self.y * self.y).sqrt()
    }
    fn norm(self) -> Pt {
        let l = self.len();
        if l < 1e-6 {
            pt(0.0, 0.0)
        } else {
            self.mul(1.0 / l)
        }
    }
    fn dot(self, o: Pt) -> f32 {
        self.x * o.x + self.y * o.y
    }
    /// Rotate by `deg` around `c`.
    pub fn rotate(self, deg: f32, c: Pt) -> Pt {
        let (s, co) = deg.to_radians().sin_cos();
        let d = self.sub(c);
        pt(c.x + d.x * co - d.y * s, c.y + d.x * s + d.y * co)
    }
}

#[derive(Clone, Debug, Default)]
pub struct Contour {
    pub pts: Vec<Pt>,
    pub closed: bool,
}

#[derive(Clone, Debug, Default)]
pub struct Path {
    pub contours: Vec<Contour>,
}

const QUAD_STEPS: usize = 14;

impl Path {
    pub fn new() -> Path {
        Path::default()
    }

    fn last(&mut self) -> &mut Contour {
        if self.contours.is_empty() {
            self.contours.push(Contour::default());
        }
        self.contours.last_mut().unwrap()
    }

    fn last_pt(&self) -> Pt {
        self.contours.last().and_then(|c| c.pts.last().copied()).unwrap_or(pt(0.0, 0.0))
    }

    pub fn move_to(mut self, x: f32, y: f32) -> Path {
        self.contours.push(Contour { pts: vec![pt(x, y)], closed: false });
        self
    }

    pub fn line_to(mut self, x: f32, y: f32) -> Path {
        self.last().pts.push(pt(x, y));
        self
    }

    pub fn quad_to(mut self, cx: f32, cy: f32, x: f32, y: f32) -> Path {
        let p0 = self.last_pt();
        let c = self.last();
        for i in 1..=QUAD_STEPS {
            let t = i as f32 / QUAD_STEPS as f32;
            let u = 1.0 - t;
            c.pts.push(pt(u * u * p0.x + 2.0 * u * t * cx + t * t * x, u * u * p0.y + 2.0 * u * t * cy + t * t * y));
        }
        self
    }

    pub fn close(mut self) -> Path {
        self.last().closed = true;
        self
    }

    pub fn polygon(pts: &[(f32, f32)]) -> Path {
        Path { contours: vec![Contour { pts: pts.iter().map(|&(x, y)| pt(x, y)).collect(), closed: true }] }
    }

    pub fn polyline(pts: &[(f32, f32)]) -> Path {
        Path { contours: vec![Contour { pts: pts.iter().map(|&(x, y)| pt(x, y)).collect(), closed: false }] }
    }

    pub fn line(x1: f32, y1: f32, x2: f32, y2: f32) -> Path {
        Path::polyline(&[(x1, y1), (x2, y2)])
    }

    pub fn rect(x: f32, y: f32, w: f32, h: f32) -> Path {
        Path::polygon(&[(x, y), (x + w, y), (x + w, y + h), (x, y + h)])
    }

    /// Rect rotated `deg` around (`cx`, `cy`).
    pub fn rect_rotated(x: f32, y: f32, w: f32, h: f32, deg: f32, cx: f32, cy: f32) -> Path {
        let c = pt(cx, cy);
        let pts = [pt(x, y), pt(x + w, y), pt(x + w, y + h), pt(x, y + h)].map(|p| p.rotate(deg, c));
        Path { contours: vec![Contour { pts: pts.to_vec(), closed: true }] }
    }

    pub fn rrect(x: f32, y: f32, w: f32, h: f32, r: f32) -> Path {
        let r = r.min(w / 2.0).min(h / 2.0);
        Path::new()
            .move_to(x + r, y)
            .line_to(x + w - r, y)
            .quad_to(x + w, y, x + w, y + r)
            .line_to(x + w, y + h - r)
            .quad_to(x + w, y + h, x + w - r, y + h)
            .line_to(x + r, y + h)
            .quad_to(x, y + h, x, y + h - r)
            .line_to(x, y + r)
            .quad_to(x, y, x + r, y)
            .close()
    }

    pub fn ellipse(cx: f32, cy: f32, rx: f32, ry: f32) -> Path {
        let n = ((rx.max(ry) * 1.5) as usize).clamp(12, 64);
        let pts = (0..n)
            .map(|i| {
                let a = i as f32 / n as f32 * core::f32::consts::TAU;
                pt(cx + rx * a.cos(), cy + ry * a.sin())
            })
            .collect();
        Path { contours: vec![Contour { pts, closed: true }] }
    }

    pub fn circle(cx: f32, cy: f32, r: f32) -> Path {
        Path::ellipse(cx, cy, r, r)
    }

    pub fn rotated(mut self, deg: f32, cx: f32, cy: f32) -> Path {
        let c = pt(cx, cy);
        for k in &mut self.contours {
            for p in &mut k.pts {
                *p = p.rotate(deg, c);
            }
        }
        self
    }

    pub fn translated(mut self, dx: f32, dy: f32) -> Path {
        for k in &mut self.contours {
            for p in &mut k.pts {
                p.x += dx;
                p.y += dy;
            }
        }
        self
    }

    /// Split every contour into dashes (`on`, `off` lengths).
    pub fn dashed(&self, on: f32, off: f32) -> Path {
        let mut out = Path::new();
        for c in &self.contours {
            let mut pts = c.pts.clone();
            if c.closed && !pts.is_empty() {
                pts.push(pts[0]);
            }
            let mut drawing = true;
            let mut left = on;
            let mut cur = vec![pts[0]];
            for w in pts.windows(2) {
                let (mut a, b) = (w[0], w[1]);
                let mut seg = b.sub(a).len();
                while seg > 0.0 {
                    if seg < left {
                        left -= seg;
                        if drawing {
                            cur.push(b);
                        }
                        seg = 0.0;
                    } else {
                        let m = a.add(b.sub(a).norm().mul(left));
                        seg -= left;
                        a = m;
                        if drawing {
                            cur.push(m);
                            out.contours.push(Contour { pts: std::mem::take(&mut cur), closed: false });
                        } else {
                            cur = vec![m];
                        }
                        drawing = !drawing;
                        left = if drawing { on } else { off };
                    }
                }
            }
            if drawing && cur.len() > 1 {
                out.contours.push(Contour { pts: cur, closed: false });
            }
        }
        out
    }

    /// Outline of this path stroked at `width`. `square` caps extend open
    /// ends by half the width.
    pub fn stroke(&self, width: f32, square: bool) -> Path {
        let hw = width / 2.0;
        let mut out = Path::new();
        for c in &self.contours {
            let mut pts: Vec<Pt> = Vec::with_capacity(c.pts.len());
            for &p in &c.pts {
                if pts.last().is_none_or(|l: &Pt| p.sub(*l).len() > 1e-3) {
                    pts.push(p);
                }
            }
            if c.closed && pts.len() > 2 && pts[0].sub(*pts.last().unwrap()).len() < 1e-3 {
                pts.pop();
            }
            let n = pts.len();
            if n < 2 {
                continue;
            }
            if c.closed && n > 2 {
                let (l, r): (Vec<Pt>, Vec<Pt>) = (0..n)
                    .map(|i| {
                        let prev = pts[(i + n - 1) % n];
                        let next = pts[(i + 1) % n];
                        offset_pair(prev, pts[i], next, hw)
                    })
                    .unzip();
                out.contours.push(Contour { pts: l, closed: true });
                out.contours.push(Contour { pts: r.into_iter().rev().collect(), closed: true });
            } else {
                if square {
                    let t0 = pts[1].sub(pts[0]).norm();
                    pts[0] = pts[0].sub(t0.mul(hw));
                    let tn = pts[n - 1].sub(pts[n - 2]).norm();
                    pts[n - 1] = pts[n - 1].add(tn.mul(hw));
                }
                let mut l = Vec::with_capacity(n);
                let mut r = Vec::with_capacity(n);
                for i in 0..n {
                    let prev = if i == 0 { pts[0].sub(pts[1].sub(pts[0])) } else { pts[i - 1] };
                    let next = if i == n - 1 { pts[i].add(pts[i].sub(pts[i - 1])) } else { pts[i + 1] };
                    let (a, b) = offset_pair(prev, pts[i], next, hw);
                    l.push(a);
                    r.push(b);
                }
                l.extend(r.into_iter().rev());
                out.contours.push(Contour { pts: l, closed: true });
            }
        }
        out
    }
}

/// Left/right offset points at `p` with a limited miter join.
fn offset_pair(prev: Pt, p: Pt, next: Pt, hw: f32) -> (Pt, Pt) {
    let t1 = p.sub(prev).norm();
    let t2 = next.sub(p).norm();
    let n1 = pt(-t1.y, t1.x);
    let n2 = pt(-t2.y, t2.x);
    let mut m = n1.add(n2).norm();
    if m.len() < 1e-6 {
        m = n2;
    }
    let k = hw / m.dot(n2).max(0.35);
    (p.add(m.mul(k)), p.sub(m.mul(k)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dash_count() {
        let d = Path::line(0.0, 0.0, 100.0, 0.0).dashed(10.0, 10.0);
        assert_eq!(d.contours.len(), 5);
    }

    #[test]
    fn stroke_closed_makes_ring() {
        let s = Path::rect(0.0, 0.0, 10.0, 10.0).stroke(2.0, false);
        assert_eq!(s.contours.len(), 2);
    }
}
