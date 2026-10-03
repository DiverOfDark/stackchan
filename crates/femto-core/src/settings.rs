//! User settings (PRD §6.6). Mirrors the design's tweak props.

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize), serde(rename_all = "lowercase"))]
pub enum Honorific {
    #[default]
    Sir,
    Madam,
    Guv,
}

impl Honorific {
    pub fn as_str(self) -> &'static str {
        match self {
            Honorific::Sir => "sir",
            Honorific::Madam => "madam",
            Honorific::Guv => "guv",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize), serde(rename_all = "kebab-case"))]
pub enum Eyewear {
    #[default]
    ExecGlasses,
    ArHalfLens,
    Reticle,
    None,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize), serde(rename_all = "kebab-case"))]
pub enum Accent {
    #[default]
    SignalRed,
    ToxicGreen,
    IceBlue,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize), serde(rename_all = "lowercase"))]
pub enum VoiceLang {
    #[default]
    Auto,
    Ru,
    En,
}

/// What the 12 body LEDs show.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize), serde(rename_all = "lowercase"))]
pub enum LedMode {
    /// Idle: session (left) / week (right) usage meters; moods animate.
    #[default]
    Usage,
    /// Moods only: a dim accent glow when idle.
    Mood,
    Off,
}

#[derive(Clone, Debug, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize), serde(default))]
pub struct Settings {
    pub name: String,
    pub honorific: Honorific,
    pub eyewear: Eyewear,
    pub corp: bool,
    pub corp_name: String,
    pub accent: Accent,
    /// Scanlines, vignette, glitch, misregistration.
    pub fx: bool,
    /// Eyes + servos follow faces.
    pub follow: bool,
    pub camera: bool,
    /// 10–100, or `None` for auto (ambient light sensor).
    pub brightness: Option<u8>,
    pub volume: u8,
    /// IANA timezone name.
    pub tz: String,
    pub voice_lang: VoiceLang,
    pub led_mode: LedMode,
    /// 0–100.
    pub led_brightness: u8,
    /// Run the meters the other way along each strip.
    pub led_flip: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            name: "Femto".into(),
            honorific: Honorific::Sir,
            eyewear: Eyewear::ExecGlasses,
            corp: true,
            corp_name: "Aldgate Dynamics".into(),
            accent: Accent::SignalRed,
            fx: true,
            follow: true,
            camera: true,
            brightness: None,
            volume: 60,
            tz: "Europe/Berlin".into(),
            voice_lang: VoiceLang::Auto,
            led_mode: LedMode::Usage,
            led_brightness: 40,
            led_flip: false,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Invalid {
    pub field: &'static str,
    pub reason: &'static str,
}

impl Settings {
    pub const NAME_MAX: usize = 12;
    pub const CORP_NAME_MAX: usize = 24;

    pub fn validate(&self) -> Result<(), Invalid> {
        let bad = |field, reason| Err(Invalid { field, reason });
        let name = self.name.trim();
        if name.is_empty() {
            return bad("name", "empty");
        }
        if name.chars().count() > Self::NAME_MAX {
            return bad("name", "longer than 12 characters");
        }
        if self.corp_name.chars().count() > Self::CORP_NAME_MAX {
            return bad("corp_name", "longer than 24 characters");
        }
        if self.brightness.is_some_and(|b| !(10..=100).contains(&b)) {
            return bad("brightness", "must be 10–100");
        }
        if self.led_brightness > 100 {
            return bad("led_brightness", "must be 0–100");
        }
        if self.volume > 100 {
            return bad("volume", "must be 0–100");
        }
        if self.tz.is_empty() {
            return bad("tz", "empty");
        }
        Ok(())
    }

    /// Up to two initials of the corp name, for the cheek mark (`AD-07`).
    pub fn corp_initials(&self) -> String {
        self.corp_name
            .split_whitespace()
            .filter_map(|w| w.chars().next())
            .take(2)
            .flat_map(char::to_uppercase)
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_valid() {
        assert_eq!(Settings::default().validate(), Ok(()));
        assert_eq!(Settings::default().corp_initials(), "AD");
    }

    #[test]
    fn rejects_long_name() {
        let s = Settings { name: "Thirteen chars".into(), ..Default::default() };
        assert_eq!(s.validate().unwrap_err().field, "name");
    }

    #[cfg(feature = "serde")]
    #[test]
    fn json_round_trip_and_partial() {
        let s: Settings = serde_json::from_str(r#"{"eyewear":"ar-half-lens","accent":"ice-blue"}"#).unwrap();
        assert_eq!(s.eyewear, Eyewear::ArHalfLens);
        assert_eq!(s.accent, Accent::IceBlue);
        assert_eq!(s.name, "Femto");
        let back: Settings = serde_json::from_str(&serde_json::to_string(&s).unwrap()).unwrap();
        assert_eq!(back, s);
    }
}
