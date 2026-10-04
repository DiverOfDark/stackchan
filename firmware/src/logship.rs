//! Ship the device log to the voice backend (`POST /api/device-logs`) so a
//! misbehaving turn can be read afterwards, not only live in the web UI.
//! Lines collect in a bounded buffer (oldest dropped when full) and go out
//! in batches every few seconds while Wi-Fi is up.

use std::sync::Mutex;
use std::time::Duration;

use anyhow::{anyhow, Result};
use embedded_svc::http::client::Client;
use esp_idf_svc::http::client::{Configuration as HttpConfig, EspHttpConnection};
use esp_idf_svc::sys;
use log::warn;

use crate::hub::HubRef;
use crate::{net, store};

/// Unsent log kept while the backend is unreachable (PSRAM).
const CAP: usize = 96 * 1024;
const EVERY: Duration = Duration::from_secs(10);
/// Largest single POST; a longer backlog goes out over several rounds.
const BATCH: usize = 32 * 1024;

static PENDING: Mutex<Vec<u8>> = Mutex::new(Vec::new());

/// Called with each chunk the log tap yields (whole lines, newline-terminated).
pub fn append(chunk: &[u8]) {
    let mut p = PENDING.lock().unwrap();
    if p.capacity() == 0 {
        p.reserve_exact(CAP);
    }
    p.extend_from_slice(chunk);
    if p.len() > CAP {
        // Drop the oldest whole lines.
        let excess = p.len() - CAP;
        let cut = p[excess..].iter().position(|&b| b == b'\n').map_or(p.len(), |i| excess + i + 1);
        p.drain(..cut);
    }
}

/// Take up to BATCH bytes of whole lines.
fn take() -> Vec<u8> {
    let mut p = PENDING.lock().unwrap();
    if p.len() <= BATCH {
        return std::mem::take(&mut *p);
    }
    let cut = p[..BATCH].iter().rposition(|&b| b == b'\n').map_or(BATCH, |i| i + 1);
    p.drain(..cut).collect()
}

/// Put an unsent batch back in front (bounded like `append`).
fn put_back(batch: Vec<u8>) {
    let mut p = PENDING.lock().unwrap();
    let rest = std::mem::take(&mut *p);
    let mut all = batch;
    all.extend_from_slice(&rest);
    if all.len() > CAP {
        let excess = all.len() - CAP;
        let cut = all[excess..].iter().position(|&b| b == b'\n').map_or(all.len(), |i| excess + i + 1);
        all.drain(..cut);
    }
    *p = all;
}

pub fn spawn(hub: HubRef) -> Result<()> {
    // Identifies this boot in the stored log: lines carry ms-since-boot.
    // SAFETY: plain FFI getter.
    let boot = unsafe { sys::esp_random() };
    crate::psram_stack_thread("logship", 8 * 1024, move || {
        let mut failing = false;
        loop {
            std::thread::sleep(EVERY);
            let (url, name, online) = {
                let h = hub.lock().unwrap();
                let url = h.store.get(store::KEY_VOICE_URL).unwrap_or_else(|| net::DEFAULT_VOICE_URL.to_owned());
                (url, h.live.name.clone(), h.net.connected)
            };
            if !online || url.is_empty() {
                continue;
            }
            // Each upload logs a line or two itself (TLS), so drain only
            // what's queued now; a full batch means there's more backlog.
            loop {
                let batch = take();
                if batch.is_empty() {
                    break;
                }
                let full = batch.len() >= BATCH / 2;
                match post(&url, &name, boot, &batch) {
                    Ok(()) => {
                        failing = false;
                        if !full {
                            break;
                        }
                    }
                    Err(e) => {
                        // Logged once per outage: the failure line itself is
                        // shipped, so don't loop on it.
                        if !failing {
                            warn!("log upload failed: {e}");
                        }
                        failing = true;
                        put_back(batch);
                        break;
                    }
                }
            }
        }
    })?;
    Ok(())
}

fn post(base: &str, device: &str, boot: u32, body: &[u8]) -> Result<()> {
    let conn = EspHttpConnection::new(&HttpConfig {
        crt_bundle_attach: Some(sys::esp_crt_bundle_attach),
        timeout: Some(Duration::from_secs(10)),
        ..Default::default()
    })?;
    let mut client = Client::wrap(conn);
    let device: String = device.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '-').collect();
    let url = format!("{}/api/device-logs?device={}&boot={boot:08x}", base.trim_end_matches('/'), device.to_lowercase());
    let len = body.len().to_string();
    let headers = [("Content-Type", "text/plain; charset=utf-8"), ("Content-Length", len.as_str())];
    let mut req = client.post(&url, &headers)?;
    embedded_svc::io::Write::write_all(&mut req, body).map_err(|e| anyhow!("{e:?}"))?;
    let resp = req.submit()?;
    match resp.status() {
        200..=299 => Ok(()),
        s => Err(anyhow!("HTTP {s}")),
    }
}
