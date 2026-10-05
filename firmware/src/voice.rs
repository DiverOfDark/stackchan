//! Voice (PRD §6.4): wake word "Эй, Фемто", WebRTC to the pipecat backend
//! and CoreS3 audio, in the femto_voice C++ component. This side maps the
//! conversation onto the engine's voice screens.
//!
//! Two sources of truth: the backend's events on the "events" data channel
//! (user/bot started/stopped speaking, live transcript, spoken TTS text)
//! drive the screens and captions when they arrive (PRD §7.2); the device's
//! own energy-based state is the fallback for a backend without them.

use std::ffi::CString;
use std::time::{Duration, Instant};

use esp_idf_svc::sys::{self, voice};
use femto_core::{Engine, Event, Settings, VoiceState};
use log::{info, warn};
use serde::Deserialize;

pub struct Voice {
    last: voice::femto_voice_state_t,
    /// Backend events seen in this turn: local state changes are ignored.
    events_this_turn: bool,
    user_text: String,
    bot_text: String,
    /// The bot finished speaking: its caption stays up until this passes
    /// (or the user speaks), so the end of a reply can still be read.
    bot_caption_until: Option<Instant>,
    /// Between user_started and user_stopped.
    user_speaking: bool,
    /// Listening for the wake word (see `set_wake_armed`).
    wake_armed: bool,
}

/// How long a finished reply's caption stays on screen.
const CAPTION_LINGER: Duration = Duration::from_secs(4);

#[derive(Deserialize)]
struct BackendEvent {
    t: String,
    #[serde(default)]
    text: String,
}

pub fn start(backend_url: &str, cfg: &Settings) -> Option<Voice> {
    if backend_url.is_empty() {
        info!("voice: no backend URL configured");
        return None;
    }
    let url = CString::new(backend_url).ok()?;
    let lang = match cfg.voice_lang {
        femto_core::settings::VoiceLang::Ru => "ru",
        femto_core::settings::VoiceLang::En => "en",
        femto_core::settings::VoiceLang::Auto => "auto",
    };
    let meta = serde_json::json!({
        "device": "femto-stackchan",
        "name": cfg.name,
        "honorific": cfg.honorific.as_str(),
        "lang": lang,
    })
    .to_string();
    let meta = CString::new(meta).ok()?;
    // SAFETY: C API; strings are copied by the callee.
    let err = unsafe { voice::femto_voice_start(url.as_ptr(), 1, cfg.volume as i32, meta.as_ptr()) };
    if err != sys::ESP_OK {
        warn!("voice start failed ({err})");
        return None;
    }
    info!("voice up (backend {})", if backend_url.is_empty() { "<unset>" } else { backend_url });
    Some(Voice { last: voice::femto_voice_state_t_FEMTO_VOICE_IDLE, events_this_turn: false, user_text: String::new(), bot_text: String::new(), bot_caption_until: None, user_speaking: false, wake_armed: true })
}

impl Voice {
    /// Push the voice state into the engine; call every frame.
    pub fn sync(&mut self, engine: &mut Engine) {
        // Backend events first: they're the precise view of the turn.
        let mut buf = [0u8; 512];
        loop {
            // SAFETY: buffer outlives the call.
            let n = unsafe { voice::femto_voice_next_event(buf.as_mut_ptr() as *mut _, buf.len()) };
            if n == 0 {
                break;
            }
            match serde_json::from_slice::<BackendEvent>(&buf[..n]) {
                Ok(ev) => self.on_event(ev, engine),
                Err(e) => warn!("bad voice event: {e}"),
            }
        }

        // SAFETY: plain getters.
        let (st, level) = unsafe { (voice::femto_voice_state(), voice::femto_voice_level()) };
        if st != self.last {
            info!("voice state {} → {}", self.last, st);
            match st {
                voice::femto_voice_state_t_FEMTO_VOICE_IDLE => {
                    self.events_this_turn = false;
                    self.user_speaking = false;
                    self.user_text.clear();
                    self.bot_text.clear();
                    engine.voice(VoiceState::Idle);
                    engine.event(Event::Answered { rationing: engine.usage_view().session_pct.unwrap_or(0) >= 85 });
                }
                _ if self.events_this_turn => {}
                voice::femto_voice_state_t_FEMTO_VOICE_CONNECTING | voice::femto_voice_state_t_FEMTO_VOICE_LISTENING => {
                    engine.voice(VoiceState::Listening(String::new()))
                }
                voice::femto_voice_state_t_FEMTO_VOICE_THINKING => engine.voice(VoiceState::Thinking),
                voice::femto_voice_state_t_FEMTO_VOICE_SPEAKING => engine.voice(VoiceState::Speaking(String::new())),
                _ => {}
            }
            self.last = st;
        }
        if self.bot_caption_until.is_some_and(|t| Instant::now() > t) {
            self.bot_caption_until = None;
            if matches!(engine.screen(), femto_core::Screen::Speaking) {
                engine.voice(VoiceState::Listening(String::new()));
            }
        }
        let speaking = matches!(engine.screen(), femto_core::Screen::Speaking);
        engine.set_mouth_level(speaking.then_some(level));
    }

    fn on_event(&mut self, ev: BackendEvent, engine: &mut Engine) {
        self.events_this_turn = true;
        match ev.t.as_str() {
            "user_started" => {
                self.user_speaking = true;
                self.bot_caption_until = None;
                if !self.bot_text.is_empty() {
                    // A new question after an answer: fresh captions.
                    self.bot_text.clear();
                    self.user_text.clear();
                }
                engine.voice(VoiceState::Listening(self.user_text.clone()));
            }
            "user_text" => {
                self.user_text = ev.text.trim().to_string();
                if matches!(engine.screen(), femto_core::Screen::Listening | femto_core::Screen::Thinking | femto_core::Screen::Face) {
                    engine.voice(VoiceState::Listening(self.user_text.clone()));
                }
            }
            "user_stopped" => {
                self.user_speaking = false;
                engine.voice(VoiceState::Thinking);
            }
            // Backend heartbeat while the LLM works: stay on the thinking
            // screen (and amber LEDs) however long the tool call takes.
            // Also after a filler ("Секунду, сэр.") finishes: the answer is
            // still coming. Never over the user talking or a reply on screen.
            "thinking" => {
                if !self.user_speaking && !matches!(engine.screen(), femto_core::Screen::Speaking) {
                    engine.voice(VoiceState::Thinking);
                }
            }
            "bot_started" => {
                self.bot_caption_until = None;
                self.bot_text.clear();
                engine.voice(VoiceState::Speaking(String::new()));
            }
            "bot_text" => {
                let word = ev.text.trim();
                if !word.is_empty() {
                    if !self.bot_text.is_empty() {
                        self.bot_text.push(' ');
                    }
                    self.bot_text.push_str(word);
                }
                engine.voice(VoiceState::Speaking(self.bot_text.clone()));
            }
            // Keep the reply readable for a moment; sync() moves on.
            "bot_stopped" => self.bot_caption_until = Some(Instant::now() + CAPTION_LINGER),
            other => info!("voice event {other}"),
        }
    }

    /// (state name, mic level 0..1) for the web UI.
    pub fn status(&self) -> (&'static str, f32) {
        // SAFETY: plain getters.
        let (st, mic) = unsafe { (voice::femto_voice_state(), voice::femto_voice_mic_level()) };
        let name = match st {
            voice::femto_voice_state_t_FEMTO_VOICE_CONNECTING => "connecting",
            voice::femto_voice_state_t_FEMTO_VOICE_LISTENING => "listening",
            voice::femto_voice_state_t_FEMTO_VOICE_THINKING => "thinking",
            voice::femto_voice_state_t_FEMTO_VOICE_SPEAKING => "speaking",
            _ => "idle",
        };
        (name, mic)
    }

    pub fn push_to_talk(&self) {
        // SAFETY: plain call.
        unsafe { voice::femto_voice_wake() };
    }

    pub fn wake_armed(&self) -> bool {
        self.wake_armed
    }

    /// Listen for the wake word only while someone's around (the UI loop
    /// decides). Push-to-talk works regardless.
    pub fn set_wake_armed(&mut self, armed: bool) {
        if armed != self.wake_armed {
            info!("wake word {}", if armed { "armed: someone's here" } else { "disarmed: nobody seen for a while" });
            self.wake_armed = armed;
            // SAFETY: plain call.
            unsafe { voice::femto_voice_set_wake_armed(armed) };
        }
    }

    pub fn set_volume(&self, v: u8) {
        // SAFETY: plain call.
        unsafe { voice::femto_voice_set_volume(v as i32) };
    }
}
