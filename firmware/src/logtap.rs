//! Rust side of the live log tap: the esp-idf-svc logger writes straight
//! to stdout (not through ESP_LOG's vprintf hook), so wrap it and copy each
//! line into the tap as well.

use std::fmt::Write;

use esp_idf_svc::log::{EspIdfLogFilter, EspLogger};
use esp_idf_svc::sys::logtap;
use log::{Log, Metadata, Record};

struct Tee(EspLogger);

static LOGGER: std::sync::OnceLock<Tee> = std::sync::OnceLock::new();

impl Log for Tee {
    fn enabled(&self, m: &Metadata) -> bool {
        self.0.enabled(m)
    }

    fn log(&self, r: &Record) {
        self.0.log(r);
        if self.enabled(r.metadata()) {
            let marker = match r.level() {
                log::Level::Error => 'E',
                log::Level::Warn => 'W',
                log::Level::Info => 'I',
                log::Level::Debug => 'D',
                log::Level::Trace => 'V',
            };
            let mut line = String::with_capacity(96);
            // SAFETY: plain getter.
            let ts = unsafe { esp_idf_svc::sys::esp_log_timestamp() };
            let _ = writeln!(line, "{marker} ({ts}) {}: {}", r.target(), r.args());
            // SAFETY: pointer/len of a live string; copied by the callee.
            unsafe { logtap::femto_logtap_push(line.as_ptr() as *const _, line.len()) };
        }
    }

    fn flush(&self) {}
}

pub fn init() {
    let tee = LOGGER.get_or_init(|| Tee(EspLogger::new(EspIdfLogFilter::new())));
    log::set_logger(tee).expect("logger set once");
    log::set_max_level(log::LevelFilter::Info);
}
