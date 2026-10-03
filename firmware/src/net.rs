//! Network task: Wi-Fi station, SNTP, and the usage poller (PRD §6.1–6.2).
//! Runs on its own thread and reports to the UI loop through a channel.

use std::sync::mpsc::Sender;
use std::time::{Duration, Instant};

use anyhow::{anyhow, Result};
use embedded_svc::http::client::Client;
use embedded_svc::wifi::{AuthMethod, ClientConfiguration, Configuration};
use esp_idf_svc::eventloop::EspSystemEventLoop;
use esp_idf_svc::hal::modem::Modem;
use esp_idf_svc::http::client::{Configuration as HttpConfig, EspHttpConnection};
use esp_idf_svc::nvs::{EspDefaultNvsPartition, EspNvs, NvsDefault};
use esp_idf_svc::sntp::{EspSntp, SyncStatus};
use esp_idf_svc::sys;
use esp_idf_svc::wifi::{BlockingWifi, EspWifi};
use femto_core::Usage;
use log::{info, warn};
use serde::Deserialize;

pub const WIFI_ATTEMPTS: u8 = 3;
/// The owner's trmnl-cyberpunk; overridable in the setup UI or with
/// `FEMTO_USAGE_URL` at build time.
const DEFAULT_USAGE_URL: &str = "https://trmnl.kirillorlov.pro";
const POLL: Duration = Duration::from_secs(60);
/// POSIX TZ for Europe/Berlin (PRD default). Settings → TZ mapping comes with the web UI.
const TZ_BERLIN: &str = "CET-1CEST,M3.5.0,M10.5.0/3";

#[derive(Debug)]
pub enum NetEvent {
    NoCredentials,
    Connecting { attempt: u8, ssid: String },
    Failed,
    Connected { ip: String },
    Clock { unix: i64, utc_offset_s: i32 },
    Usage(Usage),
    UsageError(String),
}

/// Device config in NVS namespace `femto` (written by the setup web UI).
#[derive(Clone, Debug, Default)]
pub struct NetConfig {
    pub ssid: String,
    pub pass: String,
    pub usage_url: String,
    pub usage_token: String,
}

impl NetConfig {
    pub fn load(nvs: &EspDefaultNvsPartition) -> NetConfig {
        let get = |ns: &str, key: &str| -> Option<String> {
            let store: EspNvs<NvsDefault> = EspNvs::new(nvs.clone(), ns, false).ok()?;
            let mut buf = [0u8; 256];
            store.get_str(key, &mut buf).ok().flatten().map(str::to_owned).filter(|s| !s.is_empty())
        };
        let (ssid, pass) = match get("femto", "ssid") {
            Some(s) => (s, get("femto", "pass").unwrap_or_default()),
            // Credentials left by the stock (XiaoZhi) firmware.
            None => match get("wifi", "ssid") {
                Some(s) => {
                    info!("using Wi-Fi credentials saved by the stock firmware");
                    (s, get("wifi", "password").unwrap_or_default())
                }
                None => Default::default(),
            },
        };
        NetConfig {
            ssid,
            pass,
            usage_url: get("femto", "usage_url").unwrap_or_else(|| option_env!("FEMTO_USAGE_URL").unwrap_or(DEFAULT_USAGE_URL).to_owned()),
            usage_token: get("femto", "usage_tok").or_else(|| option_env!("FEMTO_USAGE_TOKEN").map(str::to_owned)).unwrap_or_default(),
        }
    }
}

pub fn spawn(modem: Modem<'static>, nvs: EspDefaultNvsPartition, cfg: NetConfig, tx: Sender<NetEvent>) -> Result<()> {
    std::thread::Builder::new().name("net".into()).stack_size(12 * 1024).spawn(move || {
        if let Err(e) = run(modem, nvs, cfg, &tx) {
            warn!("net task: {e:?}");
        }
    })?;
    Ok(())
}

fn run(modem: Modem<'static>, nvs: EspDefaultNvsPartition, cfg: NetConfig, tx: &Sender<NetEvent>) -> Result<()> {
    if cfg.ssid.is_empty() {
        tx.send(NetEvent::NoCredentials).ok();
        return Ok(());
    }
    let sysloop = EspSystemEventLoop::take()?;
    let mut wifi = BlockingWifi::wrap(EspWifi::new(modem, sysloop.clone(), Some(nvs))?, sysloop)?;
    wifi.set_configuration(&Configuration::Client(ClientConfiguration {
        ssid: cfg.ssid.as_str().try_into().map_err(|_| anyhow!("SSID too long"))?,
        password: cfg.pass.as_str().try_into().map_err(|_| anyhow!("password too long"))?,
        auth_method: if cfg.pass.is_empty() { AuthMethod::None } else { AuthMethod::WPA2Personal },
        ..Default::default()
    }))?;
    wifi.start()?;

    let mut attempt = 0;
    loop {
        attempt += 1;
        tx.send(NetEvent::Connecting { attempt: attempt.min(WIFI_ATTEMPTS), ssid: cfg.ssid.clone() }).ok();
        match wifi.connect().and_then(|_| wifi.wait_netif_up()) {
            Ok(()) => break,
            Err(e) => {
                warn!("Wi-Fi attempt {attempt}: {e}");
                if attempt == WIFI_ATTEMPTS {
                    tx.send(NetEvent::Failed).ok();
                }
                // Keep retrying in the background (FR-2).
                std::thread::sleep(Duration::from_secs(if attempt < WIFI_ATTEMPTS { 1 } else { 10 }));
            }
        }
    }
    let ip = wifi.wifi().sta_netif().get_ip_info()?.ip.to_string();
    info!("Wi-Fi up: {ip}");
    tx.send(NetEvent::Connected { ip }).ok();

    let sntp = EspSntp::new_default().ok();
    let mut clock_set = false;

    let mut etag: Option<String> = None;
    let mut last_poll: Option<Instant> = None;
    loop {
        if last_poll.is_none_or(|t| t.elapsed() >= POLL) {
            last_poll = Some(Instant::now());
            if cfg.usage_url.is_empty() {
                tx.send(NetEvent::UsageError("no usage URL configured".into())).ok();
            } else {
                match fetch_usage(&cfg, etag.as_deref()) {
                    Ok((usage, date)) => {
                        // Until SNTP syncs, the server's Date header sets the clock.
                        if !clock_set {
                            if let Some(unix) = date {
                                send_clock(tx, unix);
                            }
                        }
                        if let Some((u, tag)) = usage {
                            etag = tag;
                            tx.send(NetEvent::Usage(u)).ok();
                        }
                    }
                    Err(e) => {
                        warn!("usage: {e:?}");
                        tx.send(NetEvent::UsageError(e.to_string())).ok();
                    }
                }
            }
        }
        if !clock_set && sntp.as_ref().is_some_and(|s| s.get_sync_status() == SyncStatus::Completed) {
            clock_set = true;
            // SAFETY: plain libc call.
            let now = unsafe { sys::time(core::ptr::null_mut()) } as i64;
            info!("SNTP synced");
            send_clock(tx, now);
        }
        if !wifi.is_connected().unwrap_or(false) {
            warn!("Wi-Fi lost, reconnecting");
            wifi.connect().and_then(|_| wifi.wait_netif_up()).ok();
        }
        std::thread::sleep(Duration::from_secs(1));
    }
}

/// Set the system clock to `unix` (if it looks unset) and report it with
/// the local UTC offset.
fn send_clock(tx: &Sender<NetEvent>, unix: i64) {
    std::env::set_var("TZ", TZ_BERLIN);
    // SAFETY: plain libc time calls.
    let off = unsafe {
        sys::tzset();
        let mut now: sys::time_t = 0;
        sys::time(&mut now);
        if (now as i64) < 1_700_000_000 {
            let tv = sys::timeval { tv_sec: unix as _, tv_usec: 0 };
            sys::settimeofday(&tv, core::ptr::null());
        }
        let t = unix as sys::time_t;
        let mut tm: sys::tm = core::mem::zeroed();
        sys::localtime_r(&t, &mut tm);
        // newlib has no tm_gmtoff: derive the offset from the broken-down time.
        local_offset(unix, &tm)
    };
    info!("clock: unix {unix}, UTC{:+}", off / 3600);
    tx.send(NetEvent::Clock { unix, utc_offset_s: off }).ok();
}

/// Offset of local time from UTC, from `tm` (local) and `unix`.
fn local_offset(unix: i64, tm: &sys::tm) -> i32 {
    let days = days_from_civil(tm.tm_year as i64 + 1900, tm.tm_mon as i64 + 1, tm.tm_mday as i64);
    let local = days * 86_400 + tm.tm_hour as i64 * 3600 + tm.tm_min as i64 * 60 + tm.tm_sec as i64;
    (local - unix) as i32
}

/// Days since 1970-01-01 (Howard Hinnant's algorithm).
fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = y.div_euclid(400);
    let yoe = y - era * 400;
    let doy = (153 * (m + if m > 2 { -3 } else { 9 }) + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

#[derive(Deserialize)]
struct UsageDto {
    signed_in: bool,
    ok: bool,
    fetched_at: Option<String>,
    session: Option<WindowDto>,
    week: Option<WindowDto>,
    limited: bool,
    back_at: Option<String>,
}

#[derive(Deserialize)]
struct WindowDto {
    pct: u8,
    resets_at: Option<String>,
}

/// Usage (`None` on 304 Not Modified) and the server's `Date` header.
type Fetched = (Option<(Usage, Option<String>)>, Option<i64>);

fn fetch_usage(cfg: &NetConfig, etag: Option<&str>) -> Result<Fetched> {
    let conn = EspHttpConnection::new(&HttpConfig {
        crt_bundle_attach: Some(sys::esp_crt_bundle_attach),
        timeout: Some(Duration::from_secs(10)),
        ..Default::default()
    })?;
    let mut client = Client::wrap(conn);
    let url = format!("{}/api/stackchan/usage", cfg.usage_url.trim_end_matches('/'));
    let auth = format!("Bearer {}", cfg.usage_token);
    let mut headers = vec![("Accept", "application/json")];
    if !cfg.usage_token.is_empty() {
        headers.push(("Authorization", auth.as_str()));
    }
    if let Some(e) = etag {
        headers.push(("If-None-Match", e));
    }
    let mut resp = client.request(embedded_svc::http::Method::Get, &url, &headers)?.submit()?;
    let status = resp.status();
    let date = resp.header("Date").and_then(parse_http_date);
    if status == 304 {
        return Ok((None, date));
    }
    if status != 200 && status != 503 {
        return Err(anyhow!("HTTP {status}"));
    }
    let tag = resp.header("ETag").map(str::to_owned);
    let mut body = Vec::new();
    let mut buf = [0u8; 512];
    loop {
        let n = embedded_svc::io::Read::read(&mut resp, &mut buf).map_err(|e| anyhow!("{e:?}"))?;
        if n == 0 {
            break;
        }
        body.extend_from_slice(&buf[..n]);
    }
    let dto: UsageDto = serde_json::from_slice(&body)?;
    Ok((Some((to_usage(dto), tag)), date))
}

fn to_usage(d: UsageDto) -> Usage {
    let t = |s: &Option<String>| s.as_deref().and_then(parse_rfc3339);
    Usage {
        signed_in: d.signed_in,
        ok: d.ok,
        fetched_at: t(&d.fetched_at),
        session_pct: d.session.as_ref().map_or(0, |w| w.pct),
        session_resets_at: d.session.as_ref().and_then(|w| t(&w.resets_at)),
        week_pct: d.week.as_ref().map_or(0, |w| w.pct),
        week_resets_at: d.week.as_ref().and_then(|w| t(&w.resets_at)),
        limited: d.limited,
        back_at: t(&d.back_at),
    }
}

/// `2026-10-03T18:20:35.006Z` / `…+02:00` → unix seconds.
fn parse_rfc3339(s: &str) -> Option<i64> {
    let b = s.as_bytes();
    let num = |a: usize, n: usize| s.get(a..a + n)?.parse::<i64>().ok();
    let (y, mo, d, h, mi, se) = (num(0, 4)?, num(5, 2)?, num(8, 2)?, num(11, 2)?, num(14, 2)?, num(17, 2)?);
    let mut i = 19;
    if b.get(i) == Some(&b'.') {
        i += 1;
        while b.get(i).is_some_and(u8::is_ascii_digit) {
            i += 1;
        }
    }
    let off = match b.get(i)? {
        b'Z' | b'z' => 0,
        sign @ (b'+' | b'-') => {
            let o = num(i + 1, 2)? * 3600 + num(i + 4, 2)? * 60;
            if *sign == b'-' { -o } else { o }
        }
        _ => return None,
    };
    Some(days_from_civil(y, mo, d) * 86_400 + h * 3600 + mi * 60 + se - off)
}

/// `Sat, 03 Oct 2026 18:23:30 GMT` → unix seconds.
fn parse_http_date(s: &str) -> Option<i64> {
    let mut it = s.split_whitespace().skip(1);
    let d: i64 = it.next()?.parse().ok()?;
    let mon = it.next()?;
    let m = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"].iter().position(|&x| x == mon)? as i64 + 1;
    let y: i64 = it.next()?.parse().ok()?;
    let mut hms = it.next()?.split(':').map(|v| v.parse::<i64>().ok());
    let (h, mi, se) = (hms.next()??, hms.next()??, hms.next()??);
    Some(days_from_civil(y, m, d) * 86_400 + h * 3600 + mi * 60 + se)
}
