//! Register-file I2C mock for tests.

use std::collections::HashMap;

use embedded_hal::i2c::{ErrorType, I2c, Operation};

#[derive(Default)]
pub struct MockI2c {
    pub regs: HashMap<(u8, u8), u8>,
    pub writes: Vec<(u8, Vec<u8>)>,
}

impl ErrorType for MockI2c {
    type Error = core::convert::Infallible;
}

impl I2c for MockI2c {
    fn transaction(&mut self, addr: u8, ops: &mut [Operation<'_>]) -> Result<(), Self::Error> {
        let mut ptr = 0u8;
        for op in ops {
            match op {
                Operation::Write(bytes) => {
                    self.writes.push((addr, bytes.to_vec()));
                    if let Some((&reg, data)) = bytes.split_first() {
                        ptr = reg;
                        for (i, &v) in data.iter().enumerate() {
                            self.regs.insert((addr, reg.wrapping_add(i as u8)), v);
                        }
                    }
                }
                Operation::Read(buf) => {
                    for (i, b) in buf.iter_mut().enumerate() {
                        *b = *self.regs.get(&(addr, ptr.wrapping_add(i as u8))).unwrap_or(&0);
                    }
                }
            }
        }
        Ok(())
    }
}
