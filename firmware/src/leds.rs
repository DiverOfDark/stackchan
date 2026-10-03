//! Body LEDs: a 25 Hz thread renders femto_core::leds from the latest UI
//! state and pushes it to the PY32 when it changes.

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use femto_core::leds::{self, LedInput};
use femto_core::settings::{Accent, LedMode};
use femto_core::{Emotion, Screen, UsageView};
use femto_drivers::py32::Py32;
use femto_render::color::Palette;
use log::warn;

use crate::board::Bus;

/// What the UI loop publishes for the LEDs.
#[derive(Clone)]
pub struct LedState {
    pub screen: Screen,
    pub em: Emotion,
    pub usage: UsageView,
    pub progress: f32,
    pub mic: f32,
    pub speak: f32,
    pub mode: LedMode,
    pub brightness: u8,
    pub flip: bool,
    pub accent: Accent,
}

impl Default for LedState {
    fn default() -> Self {
        LedState {
            screen: Screen::Boot,
            em: Emotion::Dormant,
            usage: UsageView::none(),
            progress: 0.0,
            mic: 0.0,
            speak: 0.0,
            mode: LedMode::Usage,
            brightness: 40,
            flip: false,
            accent: Accent::SignalRed,
        }
    }
}

pub type LedRef = Arc<Mutex<LedState>>;

pub fn spawn(mut body: Py32<Bus>, state: LedRef) {
    let r = crate::psram_stack_thread_prio("leds", 6144, Some(0), Some(3), move || {
        let start = Instant::now();
        let mut last = [(1u8, 1u8, 1u8); leds::COUNT];
        let mut errors = 0u32;
        loop {
            let s = state.lock().unwrap().clone();
            let pal = Palette::new(s.accent);
            let c = |x: femto_render::color::Rgb| (x.0, x.1, x.2);
            let px = leds::frame(&LedInput {
                ms: start.elapsed().as_millis() as u64,
                screen: &s.screen,
                em: s.em,
                usage: &s.usage,
                progress: s.progress,
                mic: s.mic,
                speak: s.speak,
                mode: s.mode,
                brightness: s.brightness,
                flip: s.flip,
                accent: c(pal.a),
                ink: c(pal.ink),
                toxic: c(pal.toxic),
            });
            if px != last {
                match body.show_leds(&px) {
                    Ok(()) => last = px,
                    Err(e) => {
                        errors += 1;
                        if errors % 50 == 1 {
                            warn!("LED write: {e:?}");
                        }
                    }
                }
            }
            std::thread::sleep(Duration::from_millis(40));
        }
    });
    if let Err(e) = r {
        warn!("LED thread: {e}");
    }
}
