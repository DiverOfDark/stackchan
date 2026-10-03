//! Femto firmware entry point. M0: bring-up.

use esp_idf_svc::hal::peripherals::Peripherals;
use esp_idf_svc::sys;
use log::info;

fn main() -> anyhow::Result<()> {
    sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();

    info!("femto {} booting", env!("CARGO_PKG_VERSION"));
    report_memory();

    let _peripherals = Peripherals::take()?;

    loop {
        std::thread::sleep(std::time::Duration::from_secs(5));
        report_memory();
    }
}

fn report_memory() {
    // SAFETY: plain FFI getters with no preconditions.
    let (internal, psram, psram_total) = unsafe {
        (
            sys::heap_caps_get_free_size(sys::MALLOC_CAP_INTERNAL),
            sys::heap_caps_get_free_size(sys::MALLOC_CAP_SPIRAM),
            sys::heap_caps_get_total_size(sys::MALLOC_CAP_SPIRAM),
        )
    };
    info!("heap: internal free {} KB, psram free {} / {} KB", internal / 1024, psram / 1024, psram_total / 1024);
}
