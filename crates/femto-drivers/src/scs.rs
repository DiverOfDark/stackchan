//! Feetech SCS serial bus servos (SCS0009) on the StackChan body.
//! UART 1 Mbaud, half-duplex. Big-endian 16-bit fields (SCSCL `End = 1`).
//!
//! Head geometry (from M5's BSP): id 1 = yaw, zero 460, ±128.0°;
//! id 2 = pitch, zero 620, 0..90.0°. One raw step = 0.3125°.

pub const BAUD: u32 = 1_000_000;
pub const ID_YAW: u8 = 1;
pub const ID_PITCH: u8 = 2;

const INST_READ: u8 = 0x02;
const INST_WRITE: u8 = 0x03;

pub const REG_TORQUE_ENABLE: u8 = 40;
pub const REG_GOAL_POSITION: u8 = 42;
pub const REG_PRESENT_POSITION: u8 = 56;
pub const REG_PRESENT_LOAD: u8 = 60;
pub const REG_PRESENT_VOLTAGE: u8 = 62;
pub const REG_PRESENT_TEMPERATURE: u8 = 63;
pub const REG_MOVING: u8 = 66;

/// Raw position range accepted by the BSP.
pub const RAW_MIN: u16 = 0;
pub const RAW_MAX: u16 = 1000;

/// Byte transport. The firmware implements this over the ESP-IDF UART.
pub trait Bus {
    type Error: core::fmt::Debug;
    fn write_all(&mut self, bytes: &[u8]) -> Result<(), Self::Error>;
    /// Read exactly `buf.len()` bytes or time out.
    fn read_exact(&mut self, buf: &mut [u8], timeout_ms: u32) -> Result<(), Self::Error>;
    fn clear_rx(&mut self);
}

#[derive(Debug)]
pub enum Error<E> {
    Bus(E),
    BadHeader,
    BadChecksum,
    WrongId,
    /// Servo status error byte (overheat, overload, …).
    Servo(u8),
}

fn checksum(bytes: &[u8]) -> u8 {
    !bytes.iter().fold(0u8, |a, &b| a.wrapping_add(b))
}

/// `FF FF id len inst params… chk`.
pub fn packet(id: u8, inst: u8, params: &[u8]) -> Vec<u8> {
    let mut p = Vec::with_capacity(6 + params.len());
    p.extend_from_slice(&[0xFF, 0xFF, id, params.len() as u8 + 2, inst]);
    p.extend_from_slice(params);
    p.push(checksum(&p[2..]));
    p
}

pub fn write_packet(id: u8, addr: u8, data: &[u8]) -> Vec<u8> {
    let mut params = vec![addr];
    params.extend_from_slice(data);
    packet(id, INST_WRITE, &params)
}

pub fn read_packet(id: u8, addr: u8, len: u8) -> Vec<u8> {
    packet(id, INST_READ, &[addr, len])
}

/// Angle in tenths of a degree → raw position, as the BSP maps it.
pub fn raw_from_decidegrees(zero: u16, deci: i32) -> u16 {
    (zero as i32 + deci * 16 / 50).clamp(RAW_MIN as i32, RAW_MAX as i32) as u16
}

pub fn decidegrees_from_raw(zero: u16, raw: u16) -> i32 {
    (raw as i32 - zero as i32) * 50 / 16
}

pub struct ScsBus<B> {
    bus: B,
}

impl<B: Bus> ScsBus<B> {
    pub fn new(bus: B) -> Self {
        ScsBus { bus }
    }

    fn transact(&mut self, pkt: &[u8], reply_params: usize, id: u8) -> Result<Vec<u8>, Error<B::Error>> {
        self.bus.clear_rx();
        self.bus.write_all(pkt).map_err(Error::Bus)?;
        // Status: FF FF id len err params… chk. Sync on FF FF like the
        // vendor SDK: skip up to 10 stray bytes.
        let mut prev = 0u8;
        let mut skipped = 0;
        loop {
            let mut b = [0u8];
            self.bus.read_exact(&mut b, 20).map_err(Error::Bus)?;
            if prev == 0xFF && b[0] == 0xFF {
                break;
            }
            prev = b[0];
            skipped += 1;
            if skipped > 10 {
                return Err(Error::BadHeader);
            }
        }
        let mut buf = vec![0xFFu8; 6 + reply_params];
        self.bus.read_exact(&mut buf[2..], 20).map_err(Error::Bus)?;
        if buf[2] != id {
            return Err(Error::WrongId);
        }
        let n = buf.len();
        if checksum(&buf[2..n - 1]) != buf[n - 1] {
            return Err(Error::BadChecksum);
        }
        if buf[4] != 0 {
            return Err(Error::Servo(buf[4]));
        }
        Ok(buf[5..n - 1].to_vec())
    }

    /// Writes don't wait for the status reply: on the StackChan body replies
    /// are unreliable, and waiting would cap the update rate.
    fn write(&mut self, id: u8, addr: u8, data: &[u8]) -> Result<(), Error<B::Error>> {
        self.bus.write_all(&write_packet(id, addr, data)).map_err(Error::Bus)
    }

    fn read(&mut self, id: u8, addr: u8, len: u8) -> Result<Vec<u8>, Error<B::Error>> {
        self.transact(&read_packet(id, addr, len), len as usize, id)
    }

    /// Move to `raw` in `time_ms` (0 = as fast as `speed` allows).
    pub fn write_pos(&mut self, id: u8, raw: u16, time_ms: u16, speed: u16) -> Result<(), Error<B::Error>> {
        let [ph, pl] = raw.to_be_bytes();
        let [th, tl] = time_ms.to_be_bytes();
        let [sh, sl] = speed.to_be_bytes();
        self.write(id, REG_GOAL_POSITION, &[ph, pl, th, tl, sh, sl])
    }

    pub fn torque(&mut self, id: u8, on: bool) -> Result<(), Error<B::Error>> {
        self.write(id, REG_TORQUE_ENABLE, &[on as u8])
    }

    pub fn read_pos(&mut self, id: u8) -> Result<u16, Error<B::Error>> {
        let d = self.read(id, REG_PRESENT_POSITION, 2)?;
        Ok(u16::from_be_bytes([d[0], d[1]]))
    }

    pub fn read_temperature(&mut self, id: u8) -> Result<u8, Error<B::Error>> {
        Ok(self.read(id, REG_PRESENT_TEMPERATURE, 1)?[0])
    }

    pub fn read_load(&mut self, id: u8) -> Result<i16, Error<B::Error>> {
        let d = self.read(id, REG_PRESENT_LOAD, 2)?;
        let v = u16::from_be_bytes([d[0], d[1]]);
        // Bit 10 is direction.
        let mag = (v & 0x3FF) as i16;
        Ok(if v & 0x400 != 0 { -mag } else { mag })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn torque_packet_matches_feetech() {
        // FF FF 01 04 03 28 01 CE — checksum = ~(1+4+3+0x28+1) = ~0x31.
        assert_eq!(write_packet(1, REG_TORQUE_ENABLE, &[1]), vec![0xFF, 0xFF, 0x01, 0x04, 0x03, 0x28, 0x01, 0xCE]);
    }

    #[test]
    fn read_packet_layout() {
        let p = read_packet(2, REG_PRESENT_POSITION, 2);
        assert_eq!(&p[..7], &[0xFF, 0xFF, 0x02, 0x04, 0x02, 56, 2]);
    }

    #[test]
    fn angle_mapping() {
        assert_eq!(raw_from_decidegrees(460, 0), 460);
        assert_eq!(raw_from_decidegrees(460, 900), 460 + 288);
        assert_eq!(raw_from_decidegrees(460, -2000), 0);
        assert_eq!(decidegrees_from_raw(620, 620 + 288), 900);
    }

    struct Loopback {
        reply: Vec<u8>,
        sent: Vec<u8>,
    }

    impl Bus for Loopback {
        type Error = ();
        fn write_all(&mut self, b: &[u8]) -> Result<(), ()> {
            self.sent.extend_from_slice(b);
            Ok(())
        }
        fn read_exact(&mut self, buf: &mut [u8], _: u32) -> Result<(), ()> {
            let n = buf.len();
            buf.copy_from_slice(&self.reply[..n]);
            self.reply.drain(..n);
            Ok(())
        }
        fn clear_rx(&mut self) {}
    }

    #[test]
    fn reads_position() {
        // Leading noise byte, then the status packet.
        let mut reply = vec![0x00, 0xFF, 0xFF, 0x01, 0x04, 0x00, 0x01, 0xCC, 0x00];
        let n = reply.len();
        reply[n - 1] = checksum(&reply[3..n - 1]);
        let mut bus = ScsBus::new(Loopback { reply, sent: vec![] });
        assert_eq!(bus.read_pos(1).unwrap(), 0x01CC);
    }
}
