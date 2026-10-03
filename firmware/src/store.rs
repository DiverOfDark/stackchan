//! Persistent configuration in NVS namespace `femto` (PRD §6.6–6.7).

use esp_idf_svc::nvs::{EspDefaultNvsPartition, EspNvs, NvsDefault};
use femto_core::Settings;
use log::warn;
use sha2::{Digest, Sha256};

pub const KEY_SSID: &str = "ssid";
pub const KEY_PASS: &str = "pass";
pub const KEY_USAGE_URL: &str = "usage_url";
pub const KEY_USAGE_TOKEN: &str = "usage_tok";
pub const KEY_VOICE_URL: &str = "voice_url";
const KEY_SETTINGS: &str = "settings";
const KEY_PW_HASH: &str = "pw_hash";
const KEY_PW_SALT: &str = "pw_salt";

pub struct Store {
    nvs: EspNvs<NvsDefault>,
}

impl Store {
    pub fn open(part: EspDefaultNvsPartition) -> anyhow::Result<Store> {
        Ok(Store { nvs: EspNvs::new(part, "femto", true)? })
    }

    pub fn get(&self, key: &str) -> Option<String> {
        let mut buf = [0u8; 512];
        self.nvs.get_str(key, &mut buf).ok().flatten().map(str::to_owned).filter(|s| !s.is_empty())
    }

    pub fn set(&mut self, key: &str, value: &str) {
        let r = if value.is_empty() { self.nvs.remove(key).map(|_| ()) } else { self.nvs.set_str(key, value) };
        if let Err(e) = r {
            warn!("nvs set {key}: {e}");
        }
    }

    pub fn settings(&self) -> Settings {
        self.get(KEY_SETTINGS)
            .and_then(|s| serde_json::from_str::<Settings>(&s).ok())
            .filter(|s| s.validate().is_ok())
            .unwrap_or_default()
    }

    pub fn save_settings(&mut self, s: &Settings) {
        self.set(KEY_SETTINGS, &serde_json::to_string(s).expect("settings serialize"));
    }

    pub fn has_password(&self) -> bool {
        self.get(KEY_PW_HASH).is_some()
    }

    pub fn set_password(&mut self, pw: &str) {
        if pw.is_empty() {
            self.set(KEY_PW_HASH, "");
            return;
        }
        let salt = crate::hub::random_hex(8);
        self.set(KEY_PW_SALT, &salt);
        self.set(KEY_PW_HASH, &hash(&salt, pw));
    }

    pub fn check_password(&self, pw: &str) -> bool {
        match (self.get(KEY_PW_SALT), self.get(KEY_PW_HASH)) {
            (Some(salt), Some(h)) => hash(&salt, pw) == h,
            _ => false,
        }
    }
}

fn hash(salt: &str, pw: &str) -> String {
    let d = Sha256::digest(format!("{salt}:{pw}").as_bytes());
    d.iter().map(|b| format!("{b:02x}")).collect()
}
