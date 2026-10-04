//! Device HTTP server: the embedded web UI and its JSON API (PRD §6.7).
//! Contract: `web/src/api.ts`.

use std::sync::mpsc;
use std::time::{Duration, Instant};

use anyhow::{anyhow, Result};
use embedded_svc::http::Headers;
use embedded_svc::io::Write;
use embedded_svc::ws::FrameType;
use esp_idf_svc::http::server::ws::{EspHttpWsConnection, EspHttpWsDetachedSender};
use esp_idf_svc::http::server::{Configuration, EspHttpConnection, EspHttpServer, Request};
use esp_idf_svc::http::Method;
use esp_idf_svc::ota::EspOta;
use femto_core::Settings;
use log::{info, warn};
use serde::Deserialize;
use serde_json::{json, Value};

use crate::hub::{HubRef, NetCmd, Trigger, UiCmd};
use crate::net::{self, NetConfig};
use crate::store;

static INDEX_GZ: &[u8] = include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/../web/dist/index.html.gz"));

type Req<'a, 'b> = Request<&'a mut EspHttpConnection<'b>>;

fn read_body(req: &mut Req, limit: usize) -> Result<Vec<u8>> {
    let len = req.content_len().unwrap_or(0) as usize;
    if len > limit {
        return Err(anyhow!("body too large"));
    }
    let mut body = vec![0u8; len];
    let mut n = 0;
    while n < len {
        let r = req.read(&mut body[n..]).map_err(|e| anyhow!("{e:?}"))?;
        if r == 0 {
            break;
        }
        n += r;
    }
    body.truncate(n);
    Ok(body)
}

fn json_body<T: for<'de> Deserialize<'de>>(req: &mut Req) -> Result<T> {
    Ok(serde_json::from_slice(&read_body(req, 4096)?)?)
}

fn reply(req: Req, status: u16, body: &str, ctype: &str) -> Result<()> {
    let mut r = req.into_response(status, None, &[("Content-Type", ctype), ("Cache-Control", "no-store")])?;
    r.write_all(body.as_bytes())?;
    Ok(())
}

fn ok_json(req: Req, v: &Value) -> Result<()> {
    reply(req, 200, &v.to_string(), "application/json")
}

fn no_content(req: Req) -> Result<()> {
    req.into_status_response(204)?;
    Ok(())
}

fn err(req: Req, status: u16, msg: &str) -> Result<()> {
    reply(req, status, msg, "text/plain")
}

fn session_cookie(req: &Req) -> Option<String> {
    req.header("Cookie")?
        .split(';')
        .find_map(|c| c.trim().strip_prefix("femto_session=").map(str::to_owned))
}

/// Setup mode and password-less devices are open; otherwise a session.
fn authorized(hub: &HubRef, req: &Req) -> bool {
    let h = hub.lock().unwrap();
    h.net.setup || !h.store.has_password() || session_cookie(req).is_some_and(|c| h.sessions.contains(&c))
}

/// Register a route; protected ones answer 401 without a session.
fn route<F>(server: &mut EspHttpServer<'static>, hub: &HubRef, uri: &str, method: Method, open: bool, f: F) -> Result<()>
where
    F: Fn(&HubRef, Req) -> Result<()> + Send + 'static,
{
    let hub = hub.clone();
    server.fn_handler(uri, method, move |req| -> Result<()> {
        if !open && !authorized(&hub, &req) {
            return err(req, 401, "login required");
        }
        f(&hub, req)
    })?;
    Ok(())
}

fn status_json(hub: &HubRef) -> Value {
    let h = hub.lock().unwrap();
    // SAFETY: plain heap getters.
    let (internal, psram) = unsafe {
        (
            esp_idf_svc::sys::heap_caps_get_free_size(esp_idf_svc::sys::MALLOC_CAP_INTERNAL),
            esp_idf_svc::sys::heap_caps_get_free_size(esp_idf_svc::sys::MALLOC_CAP_SPIRAM),
        )
    };
    let s = &h.snap;
    json!({
        "version": env!("CARGO_PKG_VERSION"),
        "uptime_s": h.started.elapsed().as_secs(),
        // SAFETY: plain getter. 1 power-on, 3 software, 4 panic, 5–7 watchdogs, 9 brownout.
        "reset_reason": unsafe { esp_idf_svc::sys::esp_reset_reason() },
        "setup": h.net.setup,
        "auth_required": !h.net.setup && h.store.has_password(),
        "mood": s.mood,
        "screen": s.screen,
        "panel": s.panel,
        "wifi": { "ssid": h.net.ssid, "ip": h.net.ip, "rssi": h.net.rssi, "connected": h.net.connected },
        "usage": {
            "signed_in": s.signed_in, "session_pct": s.session_pct, "week_pct": s.week_pct,
            "session_reset_min": s.session_reset_min, "stale": s.stale, "error": h.net.usage_error,
        },
        "heap": { "internal_kb": internal / 1024, "psram_kb": psram / 1024 },
        "fps": s.fps,
        "voice": { "state": s.voice_state, "mic": s.mic_level },
        "vision": { "face": s.face.map(|(x, y)| json!({ "x": x, "y": y })), "seen_s_ago": s.face_age_s },
        "dirty": h.live != h.saved,
    })
}

fn connections_json(hub: &HubRef) -> Value {
    let h = hub.lock().unwrap();
    json!({
        "usage_url": h.store.get(store::KEY_USAGE_URL).unwrap_or_else(|| net::DEFAULT_USAGE_URL.to_owned()),
        "usage_token_set": h.store.get(store::KEY_USAGE_TOKEN).is_some(),
        "voice_url": h.store.get(store::KEY_VOICE_URL).unwrap_or_else(|| net::DEFAULT_VOICE_URL.to_owned()),
    })
}

fn apply_settings(hub: &HubRef, patch: Value) -> std::result::Result<Settings, String> {
    let mut h = hub.lock().unwrap();
    let mut cur = serde_json::to_value(&h.live).expect("settings serialize");
    let (Value::Object(cur_map), Value::Object(p)) = (&mut cur, patch) else { return Err("expected a JSON object".into()) };
    for (k, v) in p {
        cur_map.insert(k, v);
    }
    let next: Settings = serde_json::from_value(cur).map_err(|e| e.to_string())?;
    next.validate().map_err(|e| format!("{}: {}", e.field, e.reason))?;
    h.live = next.clone();
    h.rev += 1;
    Ok(next)
}

pub fn start(hub: &HubRef) -> Result<EspHttpServer<'static>> {
    let mut server = EspHttpServer::new(&Configuration {
        stack_size: 12 * 1024,
        max_uri_handlers: 32,
        uri_match_wildcard: true,
        ..Default::default()
    })?;

    route(&mut server, hub, "/api/status", Method::Get, true, |hub, req| ok_json(req, &status_json(hub)))?;

    route(&mut server, hub, "/api/auth/login", Method::Post, true, |hub, mut req| {
        #[derive(Deserialize)]
        struct Login {
            password: String,
        }
        let Login { password } = json_body(&mut req)?;
        let mut h = hub.lock().unwrap();
        // 5 attempts per minute (PRD §6.7).
        while h.login_failures.front().is_some_and(|t| t.elapsed() > Duration::from_secs(60)) {
            h.login_failures.pop_front();
        }
        if h.login_failures.len() >= 5 {
            drop(h);
            return err(req, 429, "too many attempts; wait a minute");
        }
        if !h.store.check_password(&password) {
            h.login_failures.push_back(Instant::now());
            drop(h);
            return err(req, 401, "wrong password");
        }
        let token = crate::hub::random_hex(16);
        h.sessions.push_back(token.clone());
        if h.sessions.len() > 8 {
            h.sessions.pop_front();
        }
        drop(h);
        let cookie = format!("femto_session={token}; HttpOnly; SameSite=Strict; Path=/");
        req.into_response(204, None, &[("Set-Cookie", &cookie)])?;
        Ok(())
    })?;

    route(&mut server, hub, "/api/auth/password", Method::Post, false, |hub, mut req| {
        #[derive(Deserialize)]
        struct Pw {
            password: String,
        }
        let Pw { password } = json_body(&mut req)?;
        if !password.is_empty() && password.len() < 6 {
            return err(req, 422, "at least 6 characters");
        }
        let mut h = hub.lock().unwrap();
        h.store.set_password(&password);
        h.sessions.clear();
        drop(h);
        no_content(req)
    })?;

    route(&mut server, hub, "/api/settings", Method::Get, false, |hub, req| {
        let v = serde_json::to_value(&hub.lock().unwrap().live)?;
        ok_json(req, &v)
    })?;
    route(&mut server, hub, "/api/settings", Method::Put, false, |hub, mut req| {
        let patch: Value = json_body(&mut req)?;
        match apply_settings(hub, patch) {
            Ok(s) => ok_json(req, &serde_json::to_value(s)?),
            Err(e) => err(req, 422, &e),
        }
    })?;
    route(&mut server, hub, "/api/settings/save", Method::Post, false, |hub, req| {
        let mut h = hub.lock().unwrap();
        let live = h.live.clone();
        h.store.save_settings(&live);
        h.saved = live;
        drop(h);
        no_content(req)
    })?;
    route(&mut server, hub, "/api/settings/revert", Method::Post, false, |hub, req| {
        let mut h = hub.lock().unwrap();
        h.live = h.saved.clone();
        h.rev += 1;
        let v = serde_json::to_value(&h.live)?;
        drop(h);
        ok_json(req, &v)
    })?;

    route(&mut server, hub, "/api/wifi/scan", Method::Get, false, |hub, req| {
        let (tx, rx) = mpsc::channel();
        hub.lock().unwrap().net_cmd.send(NetCmd::Scan(tx)).ok();
        let nets = rx.recv_timeout(Duration::from_secs(10)).unwrap_or_default();
        ok_json(req, &serde_json::to_value(nets)?)
    })?;
    route(&mut server, hub, "/api/wifi", Method::Put, false, |hub, mut req| {
        #[derive(Deserialize)]
        struct Wifi {
            ssid: String,
            password: String,
        }
        let w: Wifi = json_body(&mut req)?;
        if w.ssid.is_empty() || w.ssid.len() > 32 || w.password.len() > 64 {
            return err(req, 422, "SSID 1–32 bytes, password up to 64");
        }
        let mut h = hub.lock().unwrap();
        h.store.set(store::KEY_SSID, &w.ssid);
        h.store.set(store::KEY_PASS, &w.password);
        let setup = h.net.setup;
        let ui = h.ui.clone();
        drop(h);
        // Outside setup, a changed network takes effect by rebooting onto it.
        if !setup {
            ui.send(UiCmd::Reboot).ok();
        }
        no_content(req)
    })?;

    route(&mut server, hub, "/api/connections", Method::Get, false, |hub, req| ok_json(req, &connections_json(hub)))?;
    route(&mut server, hub, "/api/connections", Method::Put, false, |hub, mut req| {
        #[derive(Deserialize)]
        struct Patch {
            usage_url: Option<String>,
            usage_token: Option<String>,
            voice_url: Option<String>,
        }
        let p: Patch = json_body(&mut req)?;
        {
            let mut h = hub.lock().unwrap();
            if let Some(u) = p.usage_url {
                h.store.set(store::KEY_USAGE_URL, u.trim().trim_end_matches('/'));
            }
            if let Some(t) = p.usage_token {
                h.store.set(store::KEY_USAGE_TOKEN, t.trim());
            }
            if let Some(v) = p.voice_url {
                h.store.set(store::KEY_VOICE_URL, v.trim().trim_end_matches('/'));
            }
        }
        ok_json(req, &connections_json(hub))
    })?;
    route(&mut server, hub, "/api/connections/test", Method::Post, false, |hub, req| {
        let voice = req.uri().contains("which=voice");
        let (url, token) = {
            let h = hub.lock().unwrap();
            if voice {
                (h.store.get(store::KEY_VOICE_URL).unwrap_or_else(|| net::DEFAULT_VOICE_URL.to_owned()), String::new())
            } else {
                (h.store.get(store::KEY_USAGE_URL).unwrap_or_else(|| net::DEFAULT_USAGE_URL.to_owned()), h.store.get(store::KEY_USAGE_TOKEN).unwrap_or_default())
            }
        };
        if url.is_empty() {
            return ok_json(req, &json!({ "ok": false, "status": null, "latency_ms": null, "detail": "not configured" }));
        }
        let t0 = Instant::now();
        let v = if voice {
            match net::probe(&format!("{url}/health")) {
                Ok(code) => json!({ "ok": code < 400, "status": code, "latency_ms": t0.elapsed().as_millis() as u64, "detail": if code < 400 { "reachable" } else { "unexpected status" } }),
                Err(e) => json!({ "ok": false, "status": null, "latency_ms": null, "detail": e.to_string() }),
            }
        } else {
            let cfg = NetConfig { usage_url: url, usage_token: token, ..Default::default() };
            match net::fetch_usage(&cfg, None) {
                Ok((Some((u, _)), _)) => {
                    let detail = if u.signed_in { format!("signed in · session {} %", u.session_pct) } else { "server up, no Claude account signed in".into() };
                    json!({ "ok": u.signed_in, "status": 200, "latency_ms": t0.elapsed().as_millis() as u64, "detail": detail })
                }
                Ok((None, _)) => json!({ "ok": true, "status": 304, "latency_ms": t0.elapsed().as_millis() as u64, "detail": "unchanged" }),
                Err(e) => json!({ "ok": false, "status": null, "latency_ms": null, "detail": e.to_string() }),
            }
        };
        ok_json(req, &v)
    })?;

    route(&mut server, hub, "/api/test", Method::Post, false, |hub, mut req| {
        let t: Trigger = json_body(&mut req)?;
        hub.lock().unwrap().ui.send(UiCmd::Trigger(t)).ok();
        no_content(req)
    })?;
    route(&mut server, hub, "/api/reboot", Method::Post, false, |hub, req| {
        hub.lock().unwrap().ui.send(UiCmd::Reboot).ok();
        no_content(req)
    })?;
    route(&mut server, hub, "/api/factory-reset", Method::Post, false, |hub, mut req| {
        let body: Value = json_body(&mut req)?;
        if body.get("confirm").and_then(Value::as_str) != Some("WIPE") {
            return err(req, 422, "confirm with {\"confirm\":\"WIPE\"}");
        }
        hub.lock().unwrap().ui.send(UiCmd::FactoryReset).ok();
        no_content(req)
    })?;
    route(&mut server, hub, "/api/setup/finish", Method::Post, false, |hub, req| {
        let mut h = hub.lock().unwrap();
        let live = h.live.clone();
        h.store.save_settings(&live);
        h.saved = live;
        h.ui.send(UiCmd::Reboot).ok();
        drop(h);
        no_content(req)
    })?;

    route(&mut server, hub, "/api/ota", Method::Post, false, |hub, mut req| {
        let len = req.content_len().unwrap_or(0) as usize;
        if len < 64 * 1024 {
            return err(req, 422, "that is not a firmware image");
        }
        info!("OTA: receiving {len} bytes");
        let mut ota = EspOta::new()?;
        let mut update = ota.initiate_update()?;
        let mut buf = vec![0u8; 4096];
        let mut got = 0;
        while got < len {
            let n = req.read(&mut buf).map_err(|e| anyhow!("{e:?}"))?;
            if n == 0 {
                break;
            }
            update.write(&buf[..n])?;
            got += n;
        }
        if got != len {
            update.abort()?;
            return err(req, 400, "upload cut short");
        }
        update.complete()?;
        info!("OTA: image written, rebooting");
        hub.lock().unwrap().ui.send(UiCmd::Reboot).ok();
        no_content(req)
    })?;

    route(&mut server, hub, "/api/camera.bmp", Method::Get, false, |_, req| {
        let mut raw = vec![0u8; 320 * 240 * 2];
        let (mut w, mut h) = (0u16, 0u16);
        // SAFETY: buffer and out-params live across the call.
        let n = unsafe { esp_idf_svc::sys::vision::femto_vision_last_frame(raw.as_mut_ptr(), raw.len(), &mut w, &mut h) };
        if n == 0 {
            return err(req, 404, "no camera frame yet");
        }
        let bmp = crate::vision::bmp_from_rgb565be(&raw[..n], w as usize, h as usize);
        let mut r = req.into_response(200, None, &[("Content-Type", "image/bmp"), ("Cache-Control", "no-store")])?;
        r.write_all(&bmp)?;
        Ok(())
    })?;

    route(&mut server, hub, "/api/screen.bmp", Method::Get, false, |hub, req| {
        let (tx, rx) = mpsc::channel();
        hub.lock().unwrap().screen_req = Some(tx);
        let Ok(px) = rx.recv_timeout(Duration::from_secs(2)) else {
            return err(req, 503, "renderer busy");
        };
        let bmp = crate::vision::bmp_from_rgb565(&px, femto_render::W, femto_render::H);
        let mut r = req.into_response(200, None, &[("Content-Type", "image/bmp"), ("Cache-Control", "no-store")])?;
        r.write_all(&bmp)?;
        Ok(())
    })?;

    // Live logs: WebSocket viewers get every log line while connected.
    let viewers: std::sync::Arc<std::sync::Mutex<Vec<EspHttpWsDetachedSender>>> = Default::default();
    {
        let viewers = viewers.clone();
        let hub = hub.clone();
        server.ws_handler("/api/ws/logs", None, move |ws: &mut EspHttpWsConnection| -> Result<()> {
            // ESP-IDF doesn't hand us the handshake here, so the page sends a
            // ticket (from the authenticated /api/ws/ticket) as its first frame.
            if ws.is_closed() {
                let fd = ws.session();
                let mut v = viewers.lock().unwrap();
                v.retain(|s| s.session() != fd);
                if v.is_empty() {
                    // SAFETY: plain flag set.
                    unsafe { esp_idf_svc::sys::logtap::femto_logtap_enable(false) };
                }
                return Ok(());
            }
            let mut buf = [0u8; 64];
            let (_, len) = ws.recv(&mut buf)?;
            let ticket = std::str::from_utf8(&buf[..len.min(buf.len())]).unwrap_or("").trim_end_matches('\0');
            let ok = hub.lock().unwrap().ws_tickets.remove(ticket);
            if !ok {
                return Err(anyhow!("bad ticket"));
            }
            viewers.lock().unwrap().push(ws.create_detached_sender()?);
            // SAFETY: plain flag set.
            unsafe { esp_idf_svc::sys::logtap::femto_logtap_enable(true) };
            info!("log viewer connected");
            Ok(())
        })?;
    }
    std::thread::Builder::new().name("logs-ws".into()).stack_size(4096).spawn(move || {
        let mut buf = vec![0u8; 1024];
        loop {
            // SAFETY: buffer outlives the call.
            let n = unsafe { esp_idf_svc::sys::logtap::femto_logtap_receive(buf.as_mut_ptr() as *mut _, buf.len(), 500) };
            if n == 0 {
                continue;
            }
            // Send outside the lock: a detached send waits on the httpd task,
            // which needs this lock to process a viewer's close → deadlock.
            let mut senders: Vec<EspHttpWsDetachedSender> = viewers.lock().unwrap().clone();
            let dead: Vec<i32> = senders
                .iter_mut()
                .filter_map(|s| s.send(FrameType::Text(false), &buf[..n]).is_err().then(|| s.session()))
                .collect();
            if !dead.is_empty() {
                let mut v = viewers.lock().unwrap();
                v.retain(|s| !dead.contains(&s.session()));
                if v.is_empty() {
                    // SAFETY: plain flag set.
                    unsafe { esp_idf_svc::sys::logtap::femto_logtap_enable(false) };
                }
            }
        }
    })?;

    route(&mut server, hub, "/api/tasks", Method::Get, false, |_, req| {
        // CPU share per task since boot (FreeRTOS run-time stats).
        let mut buf = vec![0u8; 4096];
        // SAFETY: buffer is large enough for the task table (~40 tasks × 40 B).
        unsafe { esp_idf_svc::sys::vTaskGetRunTimeStats(buf.as_mut_ptr() as *mut _) };
        let text = std::ffi::CStr::from_bytes_until_nul(&buf).map(|c| c.to_string_lossy().into_owned()).unwrap_or_default();
        reply(req, 200, &text, "text/plain")
    })?;

    route(&mut server, hub, "/api/ws/ticket", Method::Get, false, |hub, req| {
        let t = crate::hub::random_hex(12);
        let mut h = hub.lock().unwrap();
        if h.ws_tickets.len() > 16 {
            h.ws_tickets.clear();
        }
        h.ws_tickets.insert(t.clone());
        drop(h);
        ok_json(req, &json!({ "ticket": t }))
    })?;

    route(&mut server, hub, "/api/voice/talk", Method::Post, false, |hub, req| {
        hub.lock().unwrap().ui.send(UiCmd::Talk).ok();
        no_content(req)
    })?;

    route(&mut server, hub, "/api/motion", Method::Get, false, |hub, req| {
        let m = hub.lock().unwrap().motion.clone();
        let Some(m) = m else { return err(req, 503, "no servos (body not found)") };
        let m = m.lock().unwrap();
        let v = json!({
            "yaw": m.pos.0, "pitch": m.pos.1,
            "zero": { "yaw": m.zero.0, "pitch": m.zero.1 },
            "torque": m.torque_allowed,
            "move_ms": m.move_ms,
            "limits": { "yaw": crate::motion::YAW_LIMIT, "pitch_min": crate::motion::PITCH_MIN, "pitch_max": crate::motion::PITCH_MAX },
        });
        drop(m);
        ok_json(req, &v)
    })?;
    route(&mut server, hub, "/api/motion", Method::Put, false, |hub, mut req| {
        #[derive(Deserialize)]
        struct Patch {
            /// Whole degrees (an f32 tuple here trips an Xtensa LLVM backend bug).
            jog: Option<[i32; 2]>,
            torque: Option<bool>,
            nod: Option<bool>,
            /// Servo move time per setpoint (noise tuning).
            move_ms: Option<u16>,
        }
        let p: Patch = json_body(&mut req)?;
        let m = hub.lock().unwrap().motion.clone();
        let Some(m) = m else { return err(req, 503, "no servos (body not found)") };
        let mut m = m.lock().unwrap();
        if let Some([y, pi]) = p.jog {
            m.manual = Some((y as f32, pi as f32, Instant::now() + Duration::from_secs(15)));
        }
        if let Some(t) = p.torque {
            m.torque_allowed = t;
        }
        if let Some(ms) = p.move_ms {
            m.move_ms = ms.clamp(20, 200);
        }
        if p.nod == Some(true) {
            m.nod_until = Some(Instant::now() + Duration::from_millis(2500));
        }
        drop(m);
        no_content(req)
    })?;
    route(&mut server, hub, "/api/motion/zero", Method::Post, false, |hub, req| {
        let (m, nvs) = {
            let h = hub.lock().unwrap();
            (h.motion.clone(), h.nvs.clone())
        };
        let Some(m) = m else { return err(req, 503, "no servos (body not found)") };
        m.lock().unwrap().rezero = true;
        // Wait for the motion task to apply it, then persist in M5's keys.
        std::thread::sleep(Duration::from_millis(100));
        let (zy, zp) = m.lock().unwrap().zero;
        let mut servo: esp_idf_svc::nvs::EspNvs<esp_idf_svc::nvs::NvsDefault> = esp_idf_svc::nvs::EspNvs::new(nvs, "servo", true)?;
        servo.set_i32("zero_pos_1", zy as i32)?;
        servo.set_i32("zero_pos_2", zp as i32)?;
        ok_json(req, &json!({ "yaw": zy, "pitch": zp }))
    })?;

    // Everything else: the SPA. In setup mode, foreign hosts (captive-portal
    // probes) are redirected to the portal.
    let hub2 = hub.clone();
    server.fn_handler("/*", Method::Get, move |req| -> Result<()> {
        let setup = hub2.lock().unwrap().net.setup;
        let host = req.header("Host").unwrap_or("");
        if setup && !host.starts_with("192.168.4.1") {
            req.into_response(302, None, &[("Location", "http://192.168.4.1/#/setup")])?;
            return Ok(());
        }
        if req.uri().starts_with("/api/") {
            return err(req, 404, "no such endpoint");
        }
        let mut r = req.into_response(
            200,
            None,
            &[("Content-Type", "text/html; charset=utf-8"), ("Content-Encoding", "gzip"), ("Cache-Control", "no-cache")],
        )?;
        r.write_all(INDEX_GZ)?;
        Ok(())
    })?;

    if INDEX_GZ.len() < 1024 {
        warn!("embedded web UI looks empty");
    }
    info!("web server up ({} KB UI)", INDEX_GZ.len() / 1024);
    Ok(server)
}
