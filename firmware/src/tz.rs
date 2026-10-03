//! IANA zone (the setting) → POSIX TZ string (what newlib understands), and
//! the local UTC offset at an instant.

use esp_idf_svc::sys;

pub fn posix(iana: &str) -> &'static str {
    match iana {
        "Europe/London" => "GMT0BST,M3.5.0/1,M10.5.0",
        "Europe/Moscow" => "MSK-3",
        "Europe/Kyiv" => "EET-2EEST,M3.5.0/3,M10.5.0/4",
        "America/New_York" => "EST5EDT,M3.2.0,M11.1.0",
        "America/Los_Angeles" => "PST8PDT,M3.2.0,M11.1.0",
        "Asia/Tokyo" => "JST-9",
        "UTC" => "UTC0",
        _ => "CET-1CEST,M3.5.0,M10.5.0/3",
    }
}

/// Local offset from UTC in seconds at `unix`, for zone `iana`.
pub fn offset(iana: &str, unix: i64) -> i32 {
    std::env::set_var("TZ", posix(iana));
    // SAFETY: plain libc time calls on local values.
    let tm = unsafe {
        sys::tzset();
        let t = unix as sys::time_t;
        let mut tm: sys::tm = core::mem::zeroed();
        sys::localtime_r(&t, &mut tm);
        tm
    };
    let days = days_from_civil(tm.tm_year as i64 + 1900, tm.tm_mon as i64 + 1, tm.tm_mday as i64);
    let local = days * 86_400 + tm.tm_hour as i64 * 3600 + tm.tm_min as i64 * 60 + tm.tm_sec as i64;
    (local - unix) as i32
}

/// Days since 1970-01-01 (Howard Hinnant's algorithm).
pub fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = y.div_euclid(400);
    let yoe = y - era * 400;
    let doy = (153 * (m + if m > 2 { -3 } else { 9 }) + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}
