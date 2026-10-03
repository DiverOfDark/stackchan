//! Face tracking: camera + ESP-DL face detection (C shim in
//! components/femto_vision), ~8 Hz on its own thread (PRD §6.3).
//! Frames never leave the device (FR-14).

use std::sync::mpsc::Sender;
use std::time::Duration;

use esp_idf_svc::sys::{self, vision};
use log::{info, warn};

#[derive(Clone, Copy, Debug)]
pub enum Sight {
    /// Largest face centre, normalised −1..1 (x: viewer's right +, y: down +).
    Face { nx: f32, ny: f32, size: f32 },
    /// No face; `motion` = fraction of the frame that changed (0..1).
    Nobody { motion: f32 },
}

const W: f32 = 320.0;
const H: f32 = 240.0;

pub fn spawn(i2c_port: i32, tx: Sender<Sight>) {
    let r = crate::psram_stack_thread_on("vision", 16 * 1024, Some(1), move || {
        // SAFETY: C shim; called once, from this thread only.
        let err = unsafe { vision::femto_vision_init(i2c_port) };
        if err != sys::ESP_OK {
            return warn!("camera init failed ({err}); face tracking off");
        }
        info!("camera up, face detection running");
        let mut faces = [vision::femto_face_t { x1: 0, y1: 0, x2: 0, y2: 0, score: 0.0 }; 4];
        let mut frames = 0u32;
        let mut ms_total = 0u32;
        loop {
            let mut ms = 0u32;
            let mut motion = 0f32;
            // SAFETY: `faces` and the out-params outlive the call; max matches its length.
            let n = unsafe { vision::femto_vision_step(faces.as_mut_ptr(), faces.len() as i32, &mut ms, &mut motion) };
            if n < 0 {
                std::thread::sleep(Duration::from_millis(50));
                continue;
            }
            frames += 1;
            ms_total += ms;
            if frames % 100 == 0 {
                info!("vision: {} ms/detect avg", ms_total / 100);
                ms_total = 0;
            }
            let best = faces[..n as usize].iter().max_by_key(|f| (f.x2 - f.x1) as i32 * (f.y2 - f.y1) as i32);
            let sight = match best {
                Some(f) => Sight::Face {
                    nx: ((f.x1 + f.x2) as f32 / 2.0 / (W / 2.0) - 1.0).clamp(-1.0, 1.0),
                    ny: ((f.y1 + f.y2) as f32 / 2.0 / (H / 2.0) - 1.0).clamp(-1.0, 1.0),
                    size: (f.x2 - f.x1) as f32 / W,
                },
                None => Sight::Nobody { motion },
            };
            if tx.send(sight).is_err() {
                break;
            }
            // ~5 Hz is plenty for head tracking and leaves core 1 for audio.
            std::thread::sleep(Duration::from_millis(130));
        }
    });
    if let Err(e) = r {
        warn!("vision thread: {e}");
    }
}

/// Before any restart: tri-state the camera bus (GPIO45 is a strap pin).
pub fn stop() {
    // SAFETY: C shim; safe to call when the camera never started.
    unsafe { vision::femto_vision_stop() };
}

/// 24-bit BMP from native RGB565 pixels (the framebuffer).
pub fn bmp_from_rgb565(px: &[u16], w: usize, h: usize) -> Vec<u8> {
    let raw: Vec<u8> = px.iter().flat_map(|p| p.to_be_bytes()).collect();
    bmp_from_rgb565be(&raw, w, h)
}

/// 24-bit BMP from big-endian RGB565 (camera byte order).
pub fn bmp_from_rgb565be(raw: &[u8], w: usize, h: usize) -> Vec<u8> {
    let row = (w * 3 + 3) & !3;
    let size = 54 + row * h;
    let mut b = Vec::with_capacity(size);
    b.extend_from_slice(b"BM");
    b.extend_from_slice(&(size as u32).to_le_bytes());
    b.extend_from_slice(&[0; 4]);
    b.extend_from_slice(&54u32.to_le_bytes());
    b.extend_from_slice(&40u32.to_le_bytes());
    b.extend_from_slice(&(w as i32).to_le_bytes());
    b.extend_from_slice(&(-(h as i32)).to_le_bytes()); // top-down
    b.extend_from_slice(&1u16.to_le_bytes());
    b.extend_from_slice(&24u16.to_le_bytes());
    b.extend_from_slice(&[0; 24]);
    for y in 0..h {
        for x in 0..w {
            let i = (y * w + x) * 2;
            let p = u16::from_be_bytes([raw[i], raw[i + 1]]);
            let (r, g, bl) = (((p >> 11) & 0x1F) << 3, ((p >> 5) & 0x3F) << 2, (p & 0x1F) << 3);
            b.extend_from_slice(&[bl as u8, g as u8, r as u8]);
        }
        b.resize(b.len() + row - w * 3, 0);
    }
    b
}
