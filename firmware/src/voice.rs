//! Voice (PRD §6.4): wake word "Эй, Фемто", WebRTC to the pipecat backend
//! and CoreS3 audio, in the femto_voice C++ component. This side maps its
//! state onto the engine's voice screens.

use std::ffi::CString;

use esp_idf_svc::sys::{self, voice};
use femto_core::{Engine, Event, VoiceState};
use log::{info, warn};

pub struct Voice {
    last: voice::femto_voice_state_t,
}

pub fn start(backend_url: &str, volume: u8) -> Option<Voice> {
    let url = CString::new(backend_url).ok()?;
    // SAFETY: C API; the URL is copied by the callee.
    let err = unsafe { voice::femto_voice_start(url.as_ptr(), 1, volume as i32) };
    if err != sys::ESP_OK {
        warn!("voice start failed ({err})");
        return None;
    }
    info!("voice up (backend {})", if backend_url.is_empty() { "<unset>" } else { backend_url });
    Some(Voice { last: voice::femto_voice_state_t_FEMTO_VOICE_IDLE })
}

impl Voice {
    /// Push the voice state into the engine; call every frame.
    pub fn sync(&mut self, engine: &mut Engine) {
        // SAFETY: plain getters.
        let (st, level) = unsafe { (voice::femto_voice_state(), voice::femto_voice_level()) };
        if st != self.last {
            info!("voice state {} → {}", self.last, st);
            match st {
                voice::femto_voice_state_t_FEMTO_VOICE_CONNECTING | voice::femto_voice_state_t_FEMTO_VOICE_LISTENING => {
                    engine.voice(VoiceState::Listening(String::new()))
                }
                voice::femto_voice_state_t_FEMTO_VOICE_THINKING => engine.voice(VoiceState::Thinking),
                voice::femto_voice_state_t_FEMTO_VOICE_SPEAKING => engine.voice(VoiceState::Speaking(String::new())),
                _ => {
                    engine.voice(VoiceState::Idle);
                    engine.event(Event::Answered { rationing: engine.usage_view().session_pct.unwrap_or(0) >= 85 });
                }
            }
            self.last = st;
        }
        let speaking = st == voice::femto_voice_state_t_FEMTO_VOICE_SPEAKING;
        engine.set_mouth_level(speaking.then_some(level));
    }

    pub fn push_to_talk(&self) {
        // SAFETY: plain call.
        unsafe { voice::femto_voice_wake() };
    }

    pub fn set_volume(&self, v: u8) {
        // SAFETY: plain call.
        unsafe { voice::femto_voice_set_volume(v as i32) };
    }
}
