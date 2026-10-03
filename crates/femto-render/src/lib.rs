//! Femto's screens, drawn into a 320×240 RGB565 framebuffer.
//!
//! The same code runs in the desktop simulator, in golden-image tests and on
//! the device.

pub mod canvas;
pub mod color;
pub mod path;
pub mod scene;
pub mod text;

pub use canvas::{Canvas, H, W};
pub use scene::Renderer;
