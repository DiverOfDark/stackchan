//! The state machine: inputs in, one [`Frame`] per render out (PRD §5.2).

use crate::demo::{Demo, Step};
use crate::expr::{Emotion, Params};
use crate::settings::Settings;
use crate::text;
use crate::usage::{Usage, UsageView};
use crate::TICK_MS;

/// No face for this long → Standby.
pub const STANDBY_AFTER_MS: u64 = 20_000;
/// Boot progress bar fills over this many ticks.
const BOOT_TICKS: f32 = 46.0;

#[derive(Clone, Debug, PartialEq)]
pub enum Screen {
    Boot,
    Wifi { attempt: u8, ssid: String },
    Setup { ap_ssid: String, ap_key: String, ip: String },
    Face,
    Ledger,
    Listening,
    Thinking,
    Speaking,
    /// Factory-wipe confirmation with seconds left to confirm.
    Wipe { secs_left: u8 },
}

impl Screen {
    pub fn is_voice(&self) -> bool {
        matches!(self, Screen::Listening | Screen::Thinking | Screen::Speaking)
    }
}

/// Transient happenings that colour the mood for a moment.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Event {
    BootDone,
    HeadPat,
    NewFace,
    LoudNoise,
    PickedUp,
    QuotaAlert,
    /// A voice turn finished; `rationing` picks the follow-up mood.
    Answered { rationing: bool },
}

impl Event {
    fn mood(self) -> (Emotion, u64) {
        match self {
            Event::BootDone => (Emotion::Excited, 2_600),
            Event::HeadPat => (Emotion::Excited, 2_000),
            Event::NewFace => (Emotion::Curious, 2_000),
            Event::LoudNoise | Event::PickedUp | Event::QuotaAlert => (Emotion::Surprised, 2_000),
            Event::Answered { rationing: true } => (Emotion::Worried, 2_600),
            Event::Answered { rationing: false } => (Emotion::Happy, 2_600),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum VoiceState {
    Idle,
    /// Live transcript of the user so far.
    Listening(String),
    Thinking,
    /// Bot text being spoken.
    Speaking(String),
}

/// Everything the renderer needs for one frame.
#[derive(Clone, Debug, PartialEq)]
pub struct Frame {
    pub screen: Screen,
    pub em: Emotion,
    pub p: Params,
    /// Animation tick counter.
    pub t: u32,
    /// Caption text revealed so far.
    pub caption: String,
    pub usage: UsageView,
    /// Boot progress, 0..1.
    pub progress: f32,
}

pub struct Engine {
    now_ms: u64,
    acc_ms: u32,
    t: u32,
    cur: Params,
    screen: Screen,
    usage: Option<Usage>,
    usage_received_at: Option<i64>,
    /// (unix seconds, at engine ms) wall-clock anchor.
    wall: Option<(i64, u64)>,
    utc_offset_s: i32,
    gaze: (f32, f32),
    last_seen_ms: u64,
    presence: bool,
    follow: bool,
    override_em: Option<Emotion>,
    event_em: Option<(Emotion, u64)>,
    caption: String,
    cap_start: u32,
    boot_start: u32,
    mouth_level: Option<f32>,
    demo: Option<Demo>,
}

impl Default for Engine {
    fn default() -> Self {
        Engine::new()
    }
}

impl Engine {
    pub fn new() -> Engine {
        Engine {
            now_ms: 0,
            acc_ms: 0,
            t: 0,
            cur: Params::NEUTRAL,
            screen: Screen::Boot,
            usage: None,
            usage_received_at: None,
            wall: None,
            utc_offset_s: 0,
            gaze: (0.0, 0.0),
            last_seen_ms: 0,
            presence: true,
            follow: true,
            override_em: None,
            event_em: None,
            caption: String::new(),
            cap_start: 0,
            boot_start: 0,
            mouth_level: None,
            demo: None,
        }
    }

    // ---- inputs -----------------------------------------------------------

    pub fn apply_settings(&mut self, s: &Settings) {
        self.follow = s.follow;
        self.presence = s.camera;
    }

    pub fn set_wall_clock(&mut self, unix: i64, utc_offset_s: i32) {
        self.wall = Some((unix, self.now_ms));
        self.utc_offset_s = utc_offset_s;
    }

    pub fn unix_now(&self) -> Option<i64> {
        self.wall.map(|(u, at)| u + ((self.now_ms - at) / 1000) as i64)
    }

    pub fn set_usage(&mut self, u: Usage) {
        let was = self.usage.as_ref().map_or(0, |o| o.session_pct);
        if was < 85 && u.session_pct >= 85 {
            self.event(Event::QuotaAlert);
        }
        self.usage = Some(u);
        self.usage_received_at = self.unix_now();
    }

    pub fn usage_view(&self) -> UsageView {
        let now = self.unix_now().unwrap_or(0);
        UsageView::new(self.usage.as_ref(), now, self.usage_received_at, self.utc_offset_s)
    }

    /// A face at normalised position (−1..1, y down).
    pub fn face_seen(&mut self, nx: f32, ny: f32) {
        self.gaze = (nx.clamp(-1.0, 1.0), ny.clamp(-1.0, 1.0));
        self.last_seen_ms = self.now_ms;
    }

    pub fn face_lost(&mut self) {
        self.gaze = (0.0, 0.0);
    }

    pub fn set_screen(&mut self, s: Screen) {
        if s == Screen::Boot {
            self.boot_start = self.t;
        }
        if !s.is_voice() {
            self.caption.clear();
        }
        self.screen = s;
    }

    pub fn screen(&self) -> &Screen {
        &self.screen
    }

    /// Tap on the screen: Face ⇄ Ledger.
    pub fn tap(&mut self) {
        self.last_seen_ms = self.now_ms;
        match self.screen {
            Screen::Face => self.set_screen(Screen::Ledger),
            Screen::Ledger => self.set_screen(Screen::Face),
            _ => {}
        }
    }

    /// Force a mood (test panel). `None` = auto.
    pub fn set_override(&mut self, em: Option<Emotion>) {
        self.override_em = em;
    }

    pub fn event(&mut self, e: Event) {
        let (em, dur) = e.mood();
        self.event_em = Some((em, self.now_ms + dur));
        self.last_seen_ms = self.now_ms;
    }

    pub fn voice(&mut self, v: VoiceState) {
        self.last_seen_ms = self.now_ms;
        match v {
            VoiceState::Idle => {
                if self.screen.is_voice() {
                    self.set_screen(Screen::Face);
                }
            }
            VoiceState::Listening(text) => {
                self.screen = Screen::Listening;
                self.set_caption(text);
            }
            VoiceState::Thinking => {
                self.screen = Screen::Thinking;
                self.caption.clear();
            }
            VoiceState::Speaking(text) => {
                self.screen = Screen::Speaking;
                self.set_caption(text);
            }
        }
    }

    /// Speech playback envelope, 0..1 (`None` → synthetic flapping).
    pub fn set_mouth_level(&mut self, level: Option<f32>) {
        self.mouth_level = level;
    }

    fn set_caption(&mut self, text: String) {
        // A growing transcript keeps typing on from where it was.
        if !text.starts_with(self.caption.as_str()) || self.caption.is_empty() {
            self.cap_start = self.t;
        }
        self.caption = text;
    }

    pub fn run_demo(&mut self, s: &Settings) {
        let q = format!("{}{}", s.name, text::QUESTION_SUFFIX);
        let u = self.usage_view();
        let ans = text::usage_answer(&u, s.honorific.as_str());
        let rationing = u.session_pct.unwrap_or(0) >= 85;
        self.demo = Some(Demo::ask(self.now_ms, q, ans, rationing));
    }

    pub fn run_power_on_demo(&mut self, first_run: bool, s: &Settings) {
        self.demo = Some(Demo::power_on(self.now_ms, first_run, &s.name));
    }

    pub fn cancel_demo(&mut self) {
        self.demo = None;
    }

    // ---- time -------------------------------------------------------------

    pub fn advance(&mut self, dt_ms: u32) {
        self.now_ms += dt_ms as u64;
        self.run_demo_steps();
        if let Some((_, until)) = self.event_em {
            if self.now_ms >= until {
                self.event_em = None;
            }
        }
        self.acc_ms += dt_ms;
        while self.acc_ms >= TICK_MS {
            self.acc_ms -= TICK_MS;
            self.tick();
        }
    }

    fn run_demo_steps(&mut self) {
        let Some(mut demo) = self.demo.take() else { return };
        while let Some(step) = demo.due(self.now_ms) {
            match step {
                Step::Voice(v) => self.voice(v),
                Step::Screen(s) => self.set_screen(s),
                Step::Event(e) => self.event(e),
            }
        }
        if !demo.finished() {
            self.demo = Some(demo);
        }
    }

    fn tick(&mut self) {
        self.t += 1;
        let em = self.resolve_emotion();
        let mut tg = em.params();
        let on = self.follow && em != Emotion::Sleepy;
        tg.gx = match em {
            Emotion::Thinking => 0.7,
            _ if on => self.gaze.0,
            _ => 0.0,
        };
        tg.gy = match em {
            Emotion::Thinking => -0.9,
            Emotion::Sleepy => 0.4,
            _ if on => self.gaze.1,
            _ => 0.0,
        };
        if self.screen == Screen::Speaking {
            if let Some(level) = self.mouth_level {
                tg.mo = level.clamp(0.0, 1.0) * 6.0;
            } else if (self.t - self.cap_start) < self.caption.chars().count() as u32 {
                tg.mo = if self.t % 4 < 2 { 5.0 } else { 1.2 };
            }
        }
        self.cur.ease_toward(&tg, 0.3);
    }

    pub fn resolve_emotion(&self) -> Emotion {
        match self.screen {
            Screen::Listening => return Emotion::Listening,
            Screen::Thinking => return Emotion::Thinking,
            Screen::Speaking => return Emotion::Speaking,
            _ => {}
        }
        if let Some((em, _)) = self.event_em {
            return em;
        }
        if let Some(em) = self.override_em {
            return em;
        }
        if let Some(u) = &self.usage {
            if u.signed_in && (u.session_pct >= 85 || u.limited) {
                return Emotion::Worried;
            }
        }
        if self.presence && self.now_ms.saturating_sub(self.last_seen_ms) > STANDBY_AFTER_MS {
            return Emotion::Sleepy;
        }
        match &self.usage {
            Some(u) if u.signed_in && u.session_pct < 25 => Emotion::Happy,
            _ => Emotion::Neutral,
        }
    }

    // ---- outputs ----------------------------------------------------------

    pub fn frame(&self) -> Frame {
        let shown = self.t.saturating_sub(self.cap_start) as usize + 1;
        Frame {
            screen: self.screen.clone(),
            em: self.resolve_emotion(),
            p: self.cur,
            t: self.t,
            caption: self.caption.chars().take(shown).collect(),
            usage: self.usage_view(),
            progress: ((self.t - self.boot_start) as f32 / BOOT_TICKS).min(1.0),
        }
    }

    /// Head servo target in degrees: (pan, tilt), positive = right / up.
    pub fn head_target(&self) -> (f32, f32) {
        match self.resolve_emotion() {
            Emotion::Sleepy | Emotion::Dormant => (0.0, 0.0),
            Emotion::Thinking => (10.0, 6.0),
            Emotion::Curious => {
                // Pan double-take in place of the design's head roll.
                let wiggle = if (self.t / 6) % 2 == 0 { 3.0 } else { -3.0 };
                (self.gaze.0 * 22.0 + wiggle, -self.gaze.1 * 12.0)
            }
            _ if self.follow => (self.gaze.0 * 22.0, -self.gaze.1 * 12.0),
            _ => (0.0, 0.0),
        }
    }

    pub fn now_ms(&self) -> u64 {
        self.now_ms
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn usage(session: u8) -> Usage {
        Usage { signed_in: true, ok: true, session_pct: session, week_pct: 40, fetched_at: Some(0), ..Default::default() }
    }

    fn engine() -> Engine {
        let mut e = Engine::new();
        e.set_screen(Screen::Face);
        e.set_wall_clock(0, 0);
        e
    }

    #[test]
    fn auto_mood_rules() {
        let mut e = engine();
        e.set_usage(usage(40));
        assert_eq!(e.resolve_emotion(), Emotion::Neutral);
        e.set_usage(usage(10));
        assert_eq!(e.resolve_emotion(), Emotion::Happy);
        e.advance(STANDBY_AFTER_MS as u32 + 70);
        assert_eq!(e.resolve_emotion(), Emotion::Sleepy);
        e.face_seen(0.2, 0.0);
        assert_eq!(e.resolve_emotion(), Emotion::Happy);
    }

    #[test]
    fn crossing_85_alarms_then_rations() {
        let mut e = engine();
        e.set_usage(usage(80));
        e.set_usage(usage(90));
        assert_eq!(e.resolve_emotion(), Emotion::Surprised);
        e.advance(2_100);
        assert_eq!(e.resolve_emotion(), Emotion::Worried);
    }

    #[test]
    fn voice_overrides_everything() {
        let mut e = engine();
        e.set_usage(usage(95));
        e.voice(VoiceState::Listening("Femto".into()));
        assert_eq!(e.resolve_emotion(), Emotion::Listening);
        e.voice(VoiceState::Idle);
        assert_eq!(*e.screen(), Screen::Face);
    }

    #[test]
    fn caption_types_out() {
        let mut e = engine();
        e.voice(VoiceState::Speaking("abcdef".into()));
        assert_eq!(e.frame().caption, "a");
        e.advance(TICK_MS * 3);
        assert_eq!(e.frame().caption, "abcd");
    }

    #[test]
    fn growing_transcript_keeps_position() {
        let mut e = engine();
        e.voice(VoiceState::Listening("abc".into()));
        e.advance(TICK_MS * 10);
        e.voice(VoiceState::Listening("abc def".into()));
        assert_eq!(e.frame().caption, "abc def");
    }

    #[test]
    fn easing_converges() {
        let mut e = engine();
        e.set_override(Some(Emotion::Surprised));
        e.face_seen(0.0, 0.0);
        e.advance(TICK_MS * 40);
        assert!((e.frame().p.drop - 30.0).abs() < 0.01);
    }

    #[test]
    fn tap_toggles_ledger() {
        let mut e = engine();
        e.tap();
        assert_eq!(*e.screen(), Screen::Ledger);
        e.tap();
        assert_eq!(*e.screen(), Screen::Face);
    }

    #[test]
    fn demo_runs_to_completion() {
        let mut e = engine();
        e.set_usage(usage(38));
        e.run_demo(&Settings::default());
        assert_eq!(*e.screen(), Screen::Face);
        e.advance(10);
        assert_eq!(*e.screen(), Screen::Listening);
        e.advance(3_400);
        assert_eq!(*e.screen(), Screen::Thinking);
        e.advance(2_200);
        assert_eq!(*e.screen(), Screen::Speaking);
        e.advance(30_000);
        assert_eq!(*e.screen(), Screen::Face);
    }
}
