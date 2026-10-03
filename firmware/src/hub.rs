//! State shared by the UI loop, the network task and the web server.

use std::collections::VecDeque;
use std::sync::mpsc::Sender;
use std::sync::{Arc, Mutex};
use std::time::Instant;

use esp_idf_svc::sys;
use femto_core::Settings;
use serde::{Deserialize, Serialize};

use crate::store::Store;

pub type HubRef = Arc<Mutex<Hub>>;

#[derive(Clone, Debug, Serialize)]
pub struct NetworkInfo {
    pub ssid: String,
    pub rssi: i8,
    pub secure: bool,
}

/// Web → UI loop.
#[derive(Debug, Deserialize, Default)]
pub struct Trigger {
    pub screen: Option<String>,
    pub mood: Option<String>,
    pub demo: Option<String>,
}

#[derive(Debug)]
pub enum UiCmd {
    Trigger(Trigger),
    Reboot,
    FactoryReset,
    /// Push-to-talk from the web UI.
    Talk,
}

/// Web → network task.
pub enum NetCmd {
    Scan(Sender<Vec<NetworkInfo>>),
}

/// What the UI loop publishes each frame.
#[derive(Clone, Debug, Default)]
pub struct Snapshot {
    pub mood: String,
    pub screen: String,
    pub fps: f32,
    pub panel: String,
    pub signed_in: bool,
    pub session_pct: Option<u8>,
    pub week_pct: Option<u8>,
    pub session_reset_min: Option<u32>,
    pub stale: bool,
    /// Face tracking: last face centre (−1..1) and seconds since seen.
    pub face: Option<(f32, f32)>,
    pub face_age_s: Option<f32>,
    pub voice_state: Option<&'static str>,
    pub mic_level: f32,
}

#[derive(Clone, Debug, Default)]
pub struct NetInfo {
    pub ssid: Option<String>,
    pub ip: Option<String>,
    pub rssi: Option<i8>,
    pub connected: bool,
    /// Setup mode: SoftAP is up.
    pub setup: bool,
    pub usage_error: Option<String>,
}

pub struct Hub {
    pub live: Settings,
    pub saved: Settings,
    /// Bumped on every live change; the UI loop re-reads settings when it moves.
    pub rev: u32,
    pub store: Store,
    pub snap: Snapshot,
    pub net: NetInfo,
    pub sessions: VecDeque<String>,
    pub login_failures: VecDeque<Instant>,
    pub ui: Sender<UiCmd>,
    pub net_cmd: Sender<NetCmd>,
    pub started: Instant,
    /// Web UI screen mirror: the UI loop answers with the current frame.
    pub screen_req: Option<Sender<Vec<u16>>>,
    pub motion: Option<crate::motion::MotionRef>,
    pub nvs: esp_idf_svc::nvs::EspDefaultNvsPartition,
    /// One-time tickets for the log WebSocket (issued to logged-in pages).
    pub ws_tickets: std::collections::HashSet<String>,
}

impl Hub {
    pub fn new(store: Store, nvs: esp_idf_svc::nvs::EspDefaultNvsPartition, ui: Sender<UiCmd>, net_cmd: Sender<NetCmd>) -> HubRef {
        let saved = store.settings();
        Arc::new(Mutex::new(Hub {
            live: saved.clone(),
            saved,
            rev: 1,
            store,
            snap: Snapshot::default(),
            net: NetInfo::default(),
            sessions: VecDeque::new(),
            login_failures: VecDeque::new(),
            ui,
            net_cmd,
            started: Instant::now(),
            screen_req: None,
            motion: None,
            nvs,
            ws_tickets: Default::default(),
        }))
    }
}

pub fn random_hex(bytes: usize) -> String {
    (0..bytes)
        // SAFETY: esp_random has no preconditions (true RNG once RF is up).
        .map(|_| format!("{:02x}", unsafe { sys::esp_random() } as u8))
        .collect()
}
