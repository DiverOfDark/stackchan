//! Network task: Wi-Fi station, SNTP, and the usage poller (PRD §6.1–6.2).
//! Runs on its own thread and reports to the UI loop through a channel.

use std::sync::mpsc::{Receiver, Sender};
use std::time::{Duration, Instant};

use anyhow::{anyhow, Result};
use embedded_svc::http::client::Client;
use embedded_svc::wifi::{AccessPointConfiguration, AuthMethod, ClientConfiguration, Configuration};
use esp_idf_svc::eventloop::EspSystemEventLoop;
use esp_idf_svc::hal::modem::Modem;
use esp_idf_svc::http::client::{Configuration as HttpConfig, EspHttpConnection};
use esp_idf_svc::nvs::{EspDefaultNvsPartition, EspNvs, NvsDefault};
use esp_idf_svc::sntp::{EspSntp, SyncStatus};
use crate::tz::days_from_civil;
use esp_idf_svc::sys;
use esp_idf_svc::wifi::{BlockingWifi, EspWifi};
use femto_core::Usage;
use log::{info, warn};
use serde::Deserialize;

use crate::hub::{HubRef, NetCmd, NetworkInfo};
use crate::store;

pub const WIFI_ATTEMPTS: u8 = 3;
/// Backend defaults baked in at build time (`FEMTO_USAGE_URL` /
/// `FEMTO_VOICE_URL`, or `firmware/femto.env`); empty means "set it in the
/// setup UI". Public builds ship without them.
pub const DEFAULT_USAGE_URL: &str = match option_env!("FEMTO_USAGE_URL") {
    Some(u) => u,
    None => "",
};
pub const DEFAULT_VOICE_URL: &str = match option_env!("FEMTO_VOICE_URL") {
    Some(u) => u,
    None => "",
};
const POLL: Duration = Duration::from_secs(60);

#[derive(Debug)]
pub enum NetEvent {
    /// Setup mode: SoftAP `ssid` with WPA2 `key` at 192.168.4.1.
    Setup { ssid: String, key: String },
    Connecting { attempt: u8, ssid: String },
    Connected { ip: String },
    Clock { unix: i64 },
    Usage(Usage),
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
    pub fn load(store: &store::Store, nvs: &EspDefaultNvsPartition) -> NetConfig {
        let stock = |key: &str| -> Option<String> {
            let ns: EspNvs<NvsDefault> = EspNvs::new(nvs.clone(), "wifi", false).ok()?;
            let mut buf = [0u8; 128];
            ns.get_str(key, &mut buf).ok().flatten().map(str::to_owned).filter(|s| !s.is_empty())
        };
        let (ssid, pass) = match store.get(store::KEY_SSID) {
            Some(s) => (s, store.get(store::KEY_PASS).unwrap_or_default()),
            // Credentials left by the stock (XiaoZhi) firmware.
            None => match stock("ssid") {
                Some(s) => {
                    info!("using Wi-Fi credentials saved by the stock firmware");
                    (s, stock("password").unwrap_or_default())
                }
                None => Default::default(),
            },
        };
        NetConfig {
            ssid,
            pass,
            usage_url: store
                .get(store::KEY_USAGE_URL)
                .unwrap_or_else(|| DEFAULT_USAGE_URL.to_owned()),
            usage_token: store.get(store::KEY_USAGE_TOKEN).or_else(|| option_env!("FEMTO_USAGE_TOKEN").map(str::to_owned)).unwrap_or_default(),
        }
    }
}

pub fn spawn(modem: Modem<'static>, nvs: EspDefaultNvsPartition, cfg: NetConfig, hub: HubRef, cmds: Receiver<NetCmd>, tx: Sender<NetEvent>) -> Result<()> {
    std::thread::Builder::new().name("net".into()).stack_size(12 * 1024).spawn(move || {
        if let Err(e) = run(modem, nvs, cfg, &hub, &cmds, &tx) {
            warn!("net task: {e:?}");
        }
    })?;
    Ok(())
}

fn run(modem: Modem<'static>, nvs: EspDefaultNvsPartition, cfg: NetConfig, hub: &HubRef, cmds: &Receiver<NetCmd>, tx: &Sender<NetEvent>) -> Result<()> {
    let sysloop = EspSystemEventLoop::take()?;
    let mut wifi = BlockingWifi::wrap(EspWifi::new(modem, sysloop.clone(), Some(nvs))?, sysloop)?;

    let joined = !cfg.ssid.is_empty() && join(&mut wifi, &cfg, tx)?;
    if !joined {
        return setup_mode(&mut wifi, hub, cmds, tx);
    }
    let ip = wifi.wifi().sta_netif().get_ip_info()?.ip.to_string();
    info!("Wi-Fi up: {ip}");
    {
        let mut h = hub.lock().unwrap();
        h.net = crate::hub::NetInfo { ssid: Some(cfg.ssid.clone()), ip: Some(ip.clone()), connected: true, ..Default::default() };
    }
    tx.send(NetEvent::Connected { ip }).ok();

    let sntp = EspSntp::new_default().ok();
    let mut clock_set = false;
    let mut etag: Option<String> = None;
    let mut last_poll: Option<Instant> = None;
    loop {
        if last_poll.is_none_or(|t| t.elapsed() >= POLL) {
            last_poll = Some(Instant::now());
            let cfg = current_usage_cfg(hub, &cfg);
            let err = if cfg.usage_url.is_empty() {
                Some("no usage URL configured".to_string())
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
                        None
                    }
                    Err(e) => {
                        warn!("usage: {e:?}");
                        Some(e.to_string())
                    }
                }
            };
            hub.lock().unwrap().net.usage_error = err;
        }
        if !clock_set && sntp.as_ref().is_some_and(|s| s.get_sync_status() == SyncStatus::Completed) {
            clock_set = true;
            // SAFETY: plain libc call.
            let now = unsafe { sys::time(core::ptr::null_mut()) } as i64;
            info!("SNTP synced");
            send_clock(tx, now);
        }
        let connected = wifi.is_connected().unwrap_or(false);
        if !connected {
            warn!("Wi-Fi lost, reconnecting");
            wifi.connect().and_then(|_| wifi.wait_netif_up()).ok();
        }
        {
            let mut h = hub.lock().unwrap();
            h.net.connected = connected;
            h.net.rssi = wifi.wifi().driver().get_rssi().ok().map(|r| r as i8);
        }
        serve_cmds(&mut wifi, cmds, Duration::from_secs(1));
    }
}

/// Usage URL/token may change from the web UI without a reboot.
fn current_usage_cfg(hub: &HubRef, base: &NetConfig) -> NetConfig {
    let h = hub.lock().unwrap();
    NetConfig {
        usage_url: h.store.get(store::KEY_USAGE_URL).unwrap_or_else(|| base.usage_url.clone()),
        usage_token: h.store.get(store::KEY_USAGE_TOKEN).unwrap_or_default(),
        ..base.clone()
    }
}

/// Up to WIFI_ATTEMPTS visible attempts. `false` → go to setup.
fn join(wifi: &mut BlockingWifi<EspWifi<'static>>, cfg: &NetConfig, tx: &Sender<NetEvent>) -> Result<bool> {
    wifi.set_configuration(&Configuration::Client(ClientConfiguration {
        ssid: cfg.ssid.as_str().try_into().map_err(|_| anyhow!("SSID too long"))?,
        password: cfg.pass.as_str().try_into().map_err(|_| anyhow!("password too long"))?,
        auth_method: if cfg.pass.is_empty() { AuthMethod::None } else { AuthMethod::WPA2Personal },
        ..Default::default()
    }))?;
    wifi.start()?;
    for attempt in 1..=WIFI_ATTEMPTS {
        tx.send(NetEvent::Connecting { attempt, ssid: cfg.ssid.clone() }).ok();
        match wifi.connect().and_then(|_| wifi.wait_netif_up()) {
            Ok(()) => return Ok(true),
            Err(e) => {
                warn!("Wi-Fi attempt {attempt}: {e}");
                std::thread::sleep(Duration::from_secs(1));
            }
        }
    }
    wifi.stop()?;
    Ok(false)
}

/// SoftAP + captive DNS until the web UI finishes setup (which reboots).
fn setup_mode(wifi: &mut BlockingWifi<EspWifi<'static>>, hub: &HubRef, cmds: &Receiver<NetCmd>, tx: &Sender<NetEvent>) -> Result<()> {
    let name = hub.lock().unwrap().live.name.to_uppercase();
    let ssid = format!("{name}-SETUP");
    let key = crate::hub::random_hex(4).to_uppercase();
    let key = format!("{}-{}", &key[..4], &key[4..]);
    wifi.set_configuration(&Configuration::Mixed(
        ClientConfiguration::default(),
        AccessPointConfiguration {
            ssid: ssid.as_str().try_into().map_err(|_| anyhow!("AP SSID too long"))?,
            password: key.as_str().try_into().unwrap(),
            auth_method: AuthMethod::WPA2Personal,
            channel: 6,
            max_connections: 4,
            ..Default::default()
        },
    ))?;
    wifi.start()?;
    info!("setup mode: SoftAP {ssid} / {key}");
    {
        let mut h = hub.lock().unwrap();
        h.net = crate::hub::NetInfo { setup: true, ip: Some("192.168.4.1".into()), ..Default::default() };
    }
    tx.send(NetEvent::Setup { ssid, key }).ok();
    crate::dns::spawn([192, 168, 4, 1]);
    loop {
        serve_cmds(wifi, cmds, Duration::from_secs(1));
    }
}

fn serve_cmds(wifi: &mut BlockingWifi<EspWifi<'static>>, cmds: &Receiver<NetCmd>, wait: Duration) {
    let Ok(cmd) = cmds.recv_timeout(wait) else { return };
    match cmd {
        NetCmd::Scan(reply) => {
            let mut nets: Vec<NetworkInfo> = wifi
                .scan()
                .unwrap_or_default()
                .into_iter()
                .filter(|a| !a.ssid.is_empty())
                .map(|a| NetworkInfo { ssid: a.ssid.to_string(), rssi: a.signal_strength, secure: a.auth_method.is_some_and(|m| m != AuthMethod::None) })
                .collect();
            nets.sort_by_key(|n| std::cmp::Reverse(n.rssi));
            nets.dedup_by(|a, b| a.ssid == b.ssid);
            reply.send(nets).ok();
        }
    }
}

/// Set the system clock to `unix` if it looks unset, and report it.
fn send_clock(tx: &Sender<NetEvent>, unix: i64) {
    // SAFETY: plain libc time calls.
    unsafe {
        let mut now: sys::time_t = 0;
        sys::time(&mut now);
        if (now as i64) < 1_700_000_000 {
            let tv = sys::timeval { tv_sec: unix as _, tv_usec: 0 };
            sys::settimeofday(&tv, core::ptr::null());
        }
    }
    info!("clock: unix {unix}");
    tx.send(NetEvent::Clock { unix }).ok();
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
pub type Fetched = (Option<(Usage, Option<String>)>, Option<i64>);

pub fn fetch_usage(cfg: &NetConfig, etag: Option<&str>) -> Result<Fetched> {
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

/// GET `url`, return the status code (connection test for the voice backend).
pub fn probe(url: &str) -> Result<u16> {
    let conn = EspHttpConnection::new(&HttpConfig {
        crt_bundle_attach: Some(sys::esp_crt_bundle_attach),
        timeout: Some(Duration::from_secs(8)),
        ..Default::default()
    })?;
    let mut client = Client::wrap(conn);
    let resp = client.request(embedded_svc::http::Method::Get, url, &[])?.submit()?;
    Ok(resp.status())
}
