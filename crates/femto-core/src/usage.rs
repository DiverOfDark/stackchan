//! Claude usage as served by trmnl-cyberpunk `GET /api/stackchan/usage`.

/// Raw usage snapshot. Times are unix seconds (UTC).
#[derive(Clone, Debug, Default, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct Usage {
    pub signed_in: bool,
    /// Last upstream fetch succeeded.
    pub ok: bool,
    /// When the backend last fetched successfully.
    pub fetched_at: Option<i64>,
    pub session_pct: u8,
    pub session_resets_at: Option<i64>,
    pub week_pct: u8,
    pub week_resets_at: Option<i64>,
    pub limited: bool,
    pub back_at: Option<i64>,
}

/// Colour level of a percentage.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Level {
    /// < 60 %: bone.
    Ok,
    /// 60–84 %: toxic.
    Caution,
    /// ≥ 85 %: accent.
    Alert,
}

impl Level {
    pub fn of(pct: u8) -> Level {
        match pct {
            85.. => Level::Alert,
            60.. => Level::Caution,
            _ => Level::Ok,
        }
    }
}

/// Data is stale after this long without a successful backend fetch.
pub const STALE_AFTER_S: i64 = 6 * 60;

/// Local wall-clock instant broken down for labels.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LocalTime {
    /// 0 = Monday.
    pub weekday: u8,
    pub hour: u8,
    pub minute: u8,
}

impl LocalTime {
    pub fn from_unix(unix: i64, utc_offset_s: i32) -> LocalTime {
        let t = unix + utc_offset_s as i64;
        let days = t.div_euclid(86_400);
        let secs = t.rem_euclid(86_400);
        // 1970-01-01 was a Thursday (weekday 3 with Monday = 0).
        let weekday = (days + 3).rem_euclid(7) as u8;
        LocalTime { weekday, hour: (secs / 3600) as u8, minute: ((secs % 3600) / 60) as u8 }
    }

    pub fn weekday_short(&self) -> &'static str {
        ["MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"][self.weekday as usize]
    }

    pub fn weekday_long(&self) -> &'static str {
        ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"][self.weekday as usize]
    }
}

/// What the renderer needs: everything pre-digested for "now".
#[derive(Clone, Debug, PartialEq)]
pub struct UsageView {
    pub signed_in: bool,
    pub session_pct: Option<u8>,
    pub week_pct: Option<u8>,
    /// Minutes until the session window resets.
    pub session_reset_min: Option<u32>,
    pub week_reset: Option<LocalTime>,
    pub limited: bool,
    pub back_at: Option<LocalTime>,
    pub stale: bool,
}

impl UsageView {
    pub fn none() -> UsageView {
        UsageView {
            signed_in: false,
            session_pct: None,
            week_pct: None,
            session_reset_min: None,
            week_reset: None,
            limited: false,
            back_at: None,
            stale: true,
        }
    }

    /// `received_at` is when the device last got a response from the backend.
    pub fn new(u: Option<&Usage>, now: i64, received_at: Option<i64>, utc_offset_s: i32) -> UsageView {
        let Some(u) = u else { return UsageView::none() };
        if !u.signed_in {
            return UsageView { signed_in: false, ..UsageView::none() };
        }
        let fresh_backend = u.ok && u.fetched_at.is_some_and(|f| now - f <= STALE_AFTER_S);
        let fresh_device = received_at.is_some_and(|r| now - r <= STALE_AFTER_S);
        let mins_until = |at: i64| ((at - now).max(0) as u32).div_ceil(60);
        UsageView {
            signed_in: true,
            session_pct: Some(u.session_pct.min(100)),
            week_pct: Some(u.week_pct.min(100)),
            session_reset_min: u.session_resets_at.map(mins_until),
            week_reset: u.week_resets_at.map(|t| LocalTime::from_unix(t, utc_offset_s)),
            limited: u.limited,
            back_at: u.back_at.map(|t| LocalTime::from_unix(t, utc_offset_s)),
            stale: !(fresh_backend && fresh_device),
        }
    }

    /// The worse of session and week, for verdicts.
    pub fn worst(&self) -> Option<u8> {
        match (self.session_pct, self.week_pct) {
            (Some(a), Some(b)) => Some(a.max(b)),
            (a, b) => a.or(b),
        }
    }
}

/// `134` → `"2h 14m"` (the design's `fmt`).
pub fn fmt_minutes(m: u32) -> String {
    format!("{}h {:02}m", m / 60, m % 60)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn levels() {
        assert_eq!(Level::of(0), Level::Ok);
        assert_eq!(Level::of(59), Level::Ok);
        assert_eq!(Level::of(60), Level::Caution);
        assert_eq!(Level::of(84), Level::Caution);
        assert_eq!(Level::of(85), Level::Alert);
    }

    #[test]
    fn minutes() {
        assert_eq!(fmt_minutes(134), "2h 14m");
        assert_eq!(fmt_minutes(5), "0h 05m");
    }

    #[test]
    fn local_time() {
        // 2026-10-08T07:00:00Z is a Thursday; Berlin is UTC+2 in October.
        let t = LocalTime::from_unix(1_791_442_800, 2 * 3600);
        assert_eq!(t.weekday_short(), "THU");
        assert_eq!((t.hour, t.minute), (9, 0));
    }

    #[test]
    fn staleness() {
        let u = Usage { signed_in: true, ok: true, fetched_at: Some(1000), session_pct: 38, session_resets_at: Some(1000 + 134 * 60), week_pct: 61, ..Default::default() };
        let v = UsageView::new(Some(&u), 1000, Some(1000), 0);
        assert!(!v.stale);
        assert_eq!(v.session_reset_min, Some(134));
        assert_eq!(v.worst(), Some(61));
        let v = UsageView::new(Some(&u), 1000 + STALE_AFTER_S + 1, Some(1000), 0);
        assert!(v.stale);
        let v = UsageView::new(Some(&Usage { ok: false, ..u.clone() }), 1000, Some(1000), 0);
        assert!(v.stale);
    }

    #[test]
    fn signed_out() {
        let v = UsageView::new(Some(&Usage::default()), 0, Some(0), 0);
        assert!(!v.signed_in);
        assert_eq!(v.session_pct, None);
    }
}
