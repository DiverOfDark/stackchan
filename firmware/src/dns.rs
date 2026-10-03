//! Captive-portal DNS: answer every A query with the SoftAP address so
//! phones open the setup page by themselves (PRD FR-3).

use std::net::UdpSocket;

use log::{info, warn};

pub fn spawn(ip: [u8; 4]) {
    let r = std::thread::Builder::new().name("dns".into()).stack_size(4096).spawn(move || {
        let sock = match UdpSocket::bind("0.0.0.0:53") {
            Ok(s) => s,
            Err(e) => return warn!("captive DNS bind: {e}"),
        };
        info!("captive DNS up");
        let mut buf = [0u8; 512];
        loop {
            let Ok((n, from)) = sock.recv_from(&mut buf) else { continue };
            if let Some(reply) = answer(&buf[..n], ip) {
                sock.send_to(&reply, from).ok();
            }
        }
    });
    if let Err(e) = r {
        warn!("captive DNS thread: {e}");
    }
}

/// Build a response with one A record for the first question.
fn answer(q: &[u8], ip: [u8; 4]) -> Option<Vec<u8>> {
    if q.len() < 12 || q[2] & 0x80 != 0 {
        return None;
    }
    // End of the first question: QNAME labels, then QTYPE + QCLASS.
    let mut i = 12;
    while *q.get(i)? != 0 {
        i += 1 + q[i] as usize;
    }
    let qend = i + 5;
    let qtype = u16::from_be_bytes([*q.get(i + 1)?, *q.get(i + 2)?]);
    let mut r = Vec::with_capacity(qend + 16);
    r.extend_from_slice(&q[..2]);
    r.extend_from_slice(&[0x81, 0x80, 0, 1]); // response, RD+RA; 1 question
    r.extend_from_slice(&[0, (qtype == 1) as u8, 0, 0, 0, 0]); // answers, ns, ar
    r.extend_from_slice(q.get(12..qend)?);
    if qtype == 1 {
        r.extend_from_slice(&[0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4]);
        r.extend_from_slice(&ip);
    }
    Some(r)
}
