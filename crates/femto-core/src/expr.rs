//! Expression rig: the design's `N` / `E` tables, verbatim.

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize), serde(rename_all = "lowercase"))]
pub enum Emotion {
    /// "Contempt", the default.
    Neutral,
    /// "Satisfied". Never happy.
    Happy,
    /// "Amused".
    Excited,
    /// "Scanning".
    Curious,
    /// "Alarmed".
    Surprised,
    /// "Standby".
    Sleepy,
    /// "Rationing".
    Worried,
    /// "Processing".
    Thinking,
    Listening,
    Speaking,
    /// Asleep without the Standby decorations (boot screen before wake-up).
    Dormant,
}

impl Emotion {
    pub const MANUAL: [Emotion; 7] = [
        Emotion::Neutral,
        Emotion::Happy,
        Emotion::Excited,
        Emotion::Curious,
        Emotion::Surprised,
        Emotion::Sleepy,
        Emotion::Worried,
    ];

    /// Mood tag text shown in the status band.
    pub fn label(self) -> &'static str {
        match self {
            Emotion::Neutral => "Contempt",
            Emotion::Happy => "Satisfied",
            Emotion::Excited => "Amused",
            Emotion::Curious => "Scanning",
            Emotion::Surprised => "Alarmed",
            Emotion::Sleepy | Emotion::Dormant => "Standby",
            Emotion::Worried => "Rationing",
            Emotion::Thinking => "Processing",
            Emotion::Listening => "Listening",
            Emotion::Speaking => "Speaking",
        }
    }

    pub fn params(self) -> Params {
        let n = Params::NEUTRAL;
        match self {
            Emotion::Neutral => n,
            Emotion::Happy => Params { l_t: 6., l_b: 5., r_t: 8., r_b: 6., bli: -30., blo: -36., bri: -32., bro: -40., mw: 16., sm: 6., mo: 3., skew: 3., fang: 1., ..n },
            Emotion::Excited => Params { l_t: 18., l_b: 12., r_t: 20., r_b: 12., bli: -40., blo: -46., bri: -42., bro: -50., mw: 19., sm: 8., mo: 8., skew: 0., fang: 1., isc: 0.85, ..n },
            Emotion::Curious => Params { l_t: 7., l_b: 7., r_t: 22., r_b: 14., bli: -24., blo: -28., bri: -48., bro: -58., mw: 9., sm: -2., skew: 4., ..n },
            Emotion::Surprised => Params { l_t: 24., l_b: 16., r_t: 24., r_b: 16., bli: -48., blo: -50., bri: -50., bro: -54., mw: 7., sm: 0., mo: 8., skew: 0., drop: 30., isc: 0.6, ..n },
            Emotion::Sleepy | Emotion::Dormant => Params { slp: 1., bli: -26., blo: -26., bri: -28., bro: -28., mw: 10., sm: -1., skew: 0., ..n },
            Emotion::Worried => Params { l_t: 14., l_b: 11., r_t: 14., r_b: 11., bli: -42., blo: -30., bri: -42., bro: -30., mw: 13., sm: -6., skew: -2., ..n },
            Emotion::Thinking => Params { l_t: 10., l_b: 8., r_t: 14., r_b: 10., bli: -28., blo: -32., bri: -40., bro: -46., mw: 9., sm: -1., skew: 7., ..n },
            Emotion::Listening => Params { l_t: 13., l_b: 11., r_t: 17., r_b: 12., bli: -32., blo: -36., bri: -38., bro: -46., mw: 11., sm: 0., skew: 3., ..n },
            Emotion::Speaking => Params { l_t: 11., ..n },
        }
    }

    /// Emotions that blink.
    pub fn blinks(self) -> bool {
        matches!(
            self,
            Emotion::Neutral | Emotion::Listening | Emotion::Curious | Emotion::Worried | Emotion::Speaking | Emotion::Thinking
        )
    }
}

/// Face rig parameters. Field names follow the design (`lT` → `l_t`, …).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Params {
    /// Left eye top / bottom lid opening.
    pub l_t: f32,
    pub l_b: f32,
    /// Right eye top / bottom lid opening.
    pub r_t: f32,
    pub r_b: f32,
    /// Brow inner / outer y offsets, left then right.
    pub bli: f32,
    pub blo: f32,
    pub bri: f32,
    pub bro: f32,
    /// Mouth half-width, smile, open, skew.
    pub mw: f32,
    pub sm: f32,
    pub mo: f32,
    pub skew: f32,
    /// Sleep (eyes shut when > 0.5).
    pub slp: f32,
    /// Eyewear drop down the nose.
    pub drop: f32,
    /// Iris scale.
    pub isc: f32,
    /// Fangs visible when > 0.5.
    pub fang: f32,
    /// Gaze, −1..1.
    pub gx: f32,
    pub gy: f32,
}

impl Params {
    pub const NEUTRAL: Params = Params {
        l_t: 8., l_b: 10., r_t: 16., r_b: 11., bli: -26., blo: -34., bri: -36., bro: -46.,
        mw: 13., sm: -1., mo: 0., skew: 6., slp: 0., drop: 0., isc: 1., fang: 0., gx: 0., gy: 0.,
    };

    pub fn with_gaze(mut self, gx: f32, gy: f32) -> Self {
        self.gx = gx;
        self.gy = gy;
        self
    }

    /// Move every field `k` of the way toward `target`.
    pub fn ease_toward(&mut self, target: &Params, k: f32) {
        macro_rules! ease {
            ($($f:ident),*) => { $( self.$f += (target.$f - self.$f) * k; )* };
        }
        ease!(l_t, l_b, r_t, r_b, bli, blo, bri, bro, mw, sm, mo, skew, slp, drop, isc, fang, gx, gy);
    }
}

impl Default for Params {
    fn default() -> Self {
        Params::NEUTRAL
    }
}
