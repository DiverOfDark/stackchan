//! Femto's brain: everything that decides *what* is on screen, with no I/O.
//!
//! Time advances in design ticks of [`TICK_MS`] (70 ms), the same clock the
//! design prototype runs on, so animation constants can be copied verbatim.

pub mod demo;
pub mod engine;
pub mod expr;
pub mod settings;
pub mod text;
pub mod usage;

pub use engine::{Engine, Event, Frame, Screen, VoiceState};
pub use expr::{Emotion, Params};
pub use settings::Settings;
pub use usage::{Level, Usage, UsageView};

/// One design tick.
pub const TICK_MS: u32 = 70;
