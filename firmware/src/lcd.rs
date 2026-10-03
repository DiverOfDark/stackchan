//! ILI9342C/E over SPI3 through ESP-IDF `esp_lcd` panel IO, fed from the
//! renderer's RGB565 framebuffer in DMA-sized chunks.

use std::sync::atomic::{AtomicU32, Ordering};

use esp_idf_svc::sys::{self, esp, EspError};
use femto_drivers::ft6336::Panel;

const W: usize = 320;
const H: usize = 240;
const CHUNK_ROWS: usize = 24;
const CHUNK_PX: usize = W * CHUNK_ROWS;

const PIN_MOSI: i32 = 37;
const PIN_SCLK: i32 = 36;
const PIN_CS: i32 = 3;
const PIN_DC: i32 = 35;

/// Power, gamma and timing for the ILI9342C (M5GFX `Panel_ILI9342`).
const INIT_C: &[(u8, &[u8], u32)] = &[
    (0xC8, &[0xFF, 0x93, 0x42], 0),
    (0xC0, &[0x12, 0x12], 0),
    (0xC1, &[0x03], 0),
    (0xC5, &[0xF2], 0),
    (0xB0, &[0xE0], 0),
    (0xF6, &[0x01, 0x00, 0x00], 0),
    (0xE0, &[0x00, 0x0C, 0x11, 0x04, 0x11, 0x08, 0x37, 0x89, 0x4C, 0x06, 0x0C, 0x0A, 0x2E, 0x34, 0x0F], 0),
    (0xE1, &[0x00, 0x0B, 0x11, 0x05, 0x13, 0x09, 0x33, 0x67, 0x48, 0x07, 0x0E, 0x0B, 0x2E, 0x33, 0x0F], 0),
    (0xB6, &[0x08, 0x82, 0x1D, 0x04], 0),
];

/// ILI9342E (units after 2026-08), from M5's factory firmware.
const INIT_E: &[(u8, &[u8], u32)] = &[
    (0xDD, &[0x01], 0),
    (0xD5, &[0x00], 0),
    (0xB1, &[0x22], 0),
    (0xC8, &[0x38], 0),
    (0xCB, &[0x1C], 0),
    (0xC9, &[0x1A], 0),
    (0xCA, &[0x1A], 0),
    (0xB7, &[0x5A, 0x41, 0x11, 0x19], 0),
    (0xE4, &[0x04, 0x08, 0x11, 0x06, 0x12, 0x07, 0x3A, 0x76, 0x47, 0x07, 0x0F, 0x0A, 0x11, 0x19, 0x05], 0),
    (0xE5, &[0x02, 0x03, 0x07, 0x06, 0x12, 0x07, 0x36, 0x5F, 0x48, 0x06, 0x10, 0x0C, 0x16, 0x14, 0x09], 0),
];

/// Common tail: RGB565, BGR order, inverted (CoreS3 panel), wake, on.
const INIT_TAIL: &[(u8, &[u8], u32)] = &[
    (0x3A, &[0x55], 0),
    (0x36, &[0x08], 0),
    (0x21, &[], 0),
    (0x11, &[], 120),
    (0x29, &[], 20),
];

static DONE: AtomicU32 = AtomicU32::new(0);

unsafe extern "C" fn on_done(_: sys::esp_lcd_panel_io_handle_t, _: *mut sys::esp_lcd_panel_io_event_data_t, _: *mut core::ffi::c_void) -> bool {
    DONE.fetch_add(1, Ordering::Release);
    false
}

pub struct Lcd {
    io: sys::esp_lcd_panel_io_handle_t,
    bufs: [*mut u16; 2],
    queued: u32,
}

// The handle and DMA buffers are only touched from the render thread.
unsafe impl Send for Lcd {}

impl Lcd {
    /// Bring up SPI and the panel. Reset the panel (AW9523) before calling.
    pub fn new(panel: Panel) -> Result<Lcd, EspError> {
        let mut bus: sys::spi_bus_config_t = Default::default();
        bus.__bindgen_anon_1.mosi_io_num = PIN_MOSI;
        bus.__bindgen_anon_2.miso_io_num = -1;
        bus.sclk_io_num = PIN_SCLK;
        bus.__bindgen_anon_3.quadwp_io_num = -1;
        bus.__bindgen_anon_4.quadhd_io_num = -1;
        bus.max_transfer_sz = (CHUNK_PX * 2) as i32;
        esp!(unsafe { sys::spi_bus_initialize(sys::spi_host_device_t_SPI3_HOST, &bus, sys::spi_common_dma_t_SPI_DMA_CH_AUTO) })?;

        let mut cfg: sys::esp_lcd_panel_io_spi_config_t = Default::default();
        cfg.cs_gpio_num = PIN_CS;
        cfg.dc_gpio_num = PIN_DC;
        cfg.spi_mode = 2;
        cfg.pclk_hz = 40_000_000;
        cfg.trans_queue_depth = 4;
        cfg.lcd_cmd_bits = 8;
        cfg.lcd_param_bits = 8;
        cfg.on_color_trans_done = Some(on_done);
        let mut io = core::ptr::null_mut();
        esp!(unsafe { sys::esp_lcd_new_panel_io_spi(sys::spi_host_device_t_SPI3_HOST as sys::esp_lcd_spi_bus_handle_t, &cfg, &mut io) })?;

        let mut bufs = [core::ptr::null_mut(); 2];
        for b in &mut bufs {
            *b = unsafe { sys::heap_caps_malloc(CHUNK_PX * 2, sys::MALLOC_CAP_DMA | sys::MALLOC_CAP_INTERNAL) } as *mut u16;
            assert!(!b.is_null(), "LCD DMA buffer");
        }
        let lcd = Lcd { io, bufs, queued: 0 };
        let extra = match panel {
            Panel::Ili9342c => INIT_C,
            Panel::Ili9342e => INIT_E,
        };
        for &(cmd, data, wait) in extra.iter().chain(INIT_TAIL) {
            lcd.cmd(cmd, data)?;
            if wait > 0 {
                std::thread::sleep(std::time::Duration::from_millis(wait as u64));
            }
        }
        Ok(lcd)
    }

    fn cmd(&self, cmd: u8, data: &[u8]) -> Result<(), EspError> {
        let ptr = if data.is_empty() { core::ptr::null() } else { data.as_ptr() as *const _ };
        esp!(unsafe { sys::esp_lcd_panel_io_tx_param(self.io, cmd as i32, ptr, data.len()) })
    }

    fn wait_done(&self, n: u32) {
        while DONE.load(Ordering::Acquire).wrapping_sub(n) > u32::MAX / 2 {
            std::hint::spin_loop();
        }
    }

    /// Push a full 320×240 RGB565 frame.
    pub fn push(&mut self, fb: &[u16]) -> Result<(), EspError> {
        // Window = whole screen; RAMWR then RAMWRC for the remaining chunks.
        self.wait_done(self.queued);
        self.cmd(0x2A, &[0, 0, ((W - 1) >> 8) as u8, (W - 1) as u8])?;
        self.cmd(0x2B, &[0, 0, ((H - 1) >> 8) as u8, (H - 1) as u8])?;
        for (k, chunk) in fb.chunks(CHUNK_PX).enumerate() {
            // Buffer k%2 was last used by transfer `queued - 1` (if any).
            if k >= 2 {
                self.wait_done(self.queued - 1);
            }
            let dst = unsafe { core::slice::from_raw_parts_mut(self.bufs[k % 2], chunk.len()) };
            for (d, s) in dst.iter_mut().zip(chunk) {
                *d = s.swap_bytes();
            }
            let cmd = if k == 0 { 0x2C } else { 0x3C };
            esp!(unsafe { sys::esp_lcd_panel_io_tx_color(self.io, cmd, dst.as_ptr() as *const _, chunk.len() * 2) })?;
            self.queued = self.queued.wrapping_add(1);
        }
        Ok(())
    }
}
