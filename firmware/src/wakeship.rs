//! Upload the audio that made the wake word fire to the voice backend
//! (`POST /wake-sample`), where /wake-review labels it true or false; the
//! labelled clips retrain the model (tools/wake-word). Same wire format as
//! the original XIAO device, plus `device=femto`.

use std::time::Duration;

use anyhow::{anyhow, Result};
use embedded_svc::http::client::Client;
use esp_idf_svc::http::client::{Configuration as HttpConfig, EspHttpConnection};
use esp_idf_svc::sys::{self, voice};
use log::{info, warn};

use crate::hub::HubRef;
use crate::{net, store};

const SAMPLE_RATE: u32 = 16_000;
/// The session keeps 3 s of history.
const MAX_SAMPLES: usize = 3 * SAMPLE_RATE as usize;

pub fn spawn(hub: HubRef) -> Result<()> {
    crate::psram_stack_thread("wakeship", 8 * 1024, move || {
        let mut pcm = vec![0i16; MAX_SAMPLES];
        loop {
            std::thread::sleep(Duration::from_secs(2));
            // SAFETY: plain C struct; all-zero is valid.
            let mut meta: voice::femto_wake_sample_meta_t = unsafe { std::mem::zeroed() };
            // SAFETY: `pcm` and `meta` outlive the call; cap matches the buffer.
            let n = unsafe { voice::femto_voice_take_wake_sample(pcm.as_mut_ptr(), pcm.len(), &mut meta) };
            if n == 0 {
                continue;
            }
            let (url, online) = {
                let h = hub.lock().unwrap();
                (h.store.get(store::KEY_VOICE_URL).unwrap_or_else(|| net::DEFAULT_VOICE_URL.to_owned()), h.net.connected)
            };
            if !online || url.is_empty() {
                continue;
            }
            match post(&url, &pcm[..n], &meta) {
                Ok(()) => info!("wake sample #{} uploaded ({:.1} s)", meta.fire_seq, n as f32 / SAMPLE_RATE as f32),
                Err(e) => warn!("wake sample upload failed: {e}"),
            }
        }
    })?;
    Ok(())
}

fn wav(pcm: &[i16]) -> Vec<u8> {
    let data = (pcm.len() * 2) as u32;
    let mut w = Vec::with_capacity(44 + data as usize);
    w.extend_from_slice(b"RIFF");
    w.extend_from_slice(&(36 + data).to_le_bytes());
    w.extend_from_slice(b"WAVEfmt ");
    w.extend_from_slice(&16u32.to_le_bytes());
    w.extend_from_slice(&1u16.to_le_bytes()); // PCM
    w.extend_from_slice(&1u16.to_le_bytes()); // mono
    w.extend_from_slice(&SAMPLE_RATE.to_le_bytes());
    w.extend_from_slice(&(SAMPLE_RATE * 2).to_le_bytes());
    w.extend_from_slice(&2u16.to_le_bytes());
    w.extend_from_slice(&16u16.to_le_bytes());
    w.extend_from_slice(b"data");
    w.extend_from_slice(&data.to_le_bytes());
    for s in pcm {
        w.extend_from_slice(&s.to_le_bytes());
    }
    w
}

fn post(base: &str, pcm: &[i16], m: &voice::femto_wake_sample_meta_t) -> Result<()> {
    let conn = EspHttpConnection::new(&HttpConfig {
        crt_bundle_attach: Some(sys::esp_crt_bundle_attach),
        timeout: Some(Duration::from_secs(15)),
        ..Default::default()
    })?;
    let mut client = Client::wrap(conn);
    let w = m.window;
    let url = format!(
        "{}/wake-sample?seq={}&peak={:.3}&avg={:.3}&hits={}&win={:.3},{:.3},{:.3},{:.3},{:.3}&sr={SAMPLE_RATE}&samples={}&uptime={}&device=femto",
        base.trim_end_matches('/'),
        m.fire_seq,
        m.peak,
        m.avg,
        m.hits,
        w[0],
        w[1],
        w[2],
        w[3],
        w[4],
        pcm.len(),
        m.uptime_ms
    );
    let body = wav(pcm);
    let len = body.len().to_string();
    let headers = [("Content-Type", "audio/wav"), ("Content-Length", len.as_str())];
    let mut req = client.post(&url, &headers)?;
    embedded_svc::io::Write::write_all(&mut req, &body).map_err(|e| anyhow!("{e:?}"))?;
    let resp = req.submit()?;
    match resp.status() {
        200..=299 => Ok(()),
        s => Err(anyhow!("HTTP {s}")),
    }
}
