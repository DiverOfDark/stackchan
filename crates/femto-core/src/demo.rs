//! Scripted flows from the design's "Try it" panel.

use crate::engine::{Event, Screen, VoiceState};

pub(crate) enum Step {
    Voice(VoiceState),
    Screen(Screen),
    Event(Event),
}

pub(crate) struct Demo {
    steps: Vec<(u64, Step)>,
    next: usize,
}

impl Demo {
    /// "Femto, how much Claude do I have left?"
    pub fn ask(now: u64, question: String, answer: String, rationing: bool) -> Demo {
        let speak = answer.chars().count() as u64 * 70 + 2_200;
        let steps = vec![
            (now, Step::Voice(VoiceState::Listening(question))),
            (now + 3_400, Step::Voice(VoiceState::Thinking)),
            (now + 5_600, Step::Voice(VoiceState::Speaking(answer))),
            (now + 5_600 + speak, Step::Voice(VoiceState::Idle)),
            (now + 5_600 + speak, Step::Event(Event::Answered { rationing })),
        ];
        Demo { steps, next: 0 }
    }

    /// Power on, with or without saved Wi-Fi.
    pub fn power_on(now: u64, first_run: bool, name: &str) -> Demo {
        let mut steps = vec![(now, Step::Screen(Screen::Boot))];
        if first_run {
            steps.push((
                now + 3_600,
                Step::Screen(Screen::Setup {
                    ap_ssid: format!("{}-SETUP", name.to_uppercase()),
                    ap_key: "7F3A-9C21".into(),
                    ip: "192.168.4.1".into(),
                }),
            ));
        } else {
            let wifi = |attempt| Step::Screen(Screen::Wifi { attempt, ssid: "HOME-WIFI".into() });
            steps.push((now + 3_600, wifi(1)));
            steps.push((now + 5_400, wifi(2)));
            steps.push((now + 7_200, Step::Screen(Screen::Face)));
            steps.push((now + 7_200, Step::Event(Event::BootDone)));
        }
        Demo { steps, next: 0 }
    }

    pub fn due(&mut self, now: u64) -> Option<Step> {
        let (at, _) = self.steps.get(self.next)?;
        if *at > now {
            return None;
        }
        self.next += 1;
        // Steps are consumed once; swap out to avoid cloning.
        let (_, step) = std::mem::replace(&mut self.steps[self.next - 1], (0, Step::Event(Event::NewFace)));
        Some(step)
    }

    pub fn finished(&self) -> bool {
        self.next >= self.steps.len()
    }
}
