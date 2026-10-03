//! Copy: verdicts, quips and caption wrapping.

use crate::usage::{fmt_minutes, UsageView};

/// Word-wrap to `n` chars per line, keeping only the last two lines
/// (the design's `wrap`).
pub fn wrap_last_two(s: &str, n: usize) -> Vec<String> {
    let mut lines = Vec::new();
    let mut cur = String::new();
    for w in s.split(' ') {
        let candidate_len = if cur.is_empty() { w.chars().count() } else { cur.chars().count() + 1 + w.chars().count() };
        if candidate_len > n && !cur.is_empty() {
            lines.push(std::mem::take(&mut cur));
            cur.push_str(w);
        } else {
            if !cur.is_empty() {
                cur.push(' ');
            }
            cur.push_str(w);
        }
    }
    if !cur.trim().is_empty() {
        lines.push(cur);
    }
    let skip = lines.len().saturating_sub(2);
    lines.into_iter().skip(skip).collect()
}

/// The Ledger's verdict line (PRD FR-9, FR-11).
pub fn ledger_verdict(u: &UsageView, hon: &str) -> String {
    if !u.signed_in {
        return format!("Nobody has authorised my ledger, {hon}.");
    }
    if u.limited {
        return match u.back_at {
            Some(t) => format!("Locked out until {:02}:{:02}. Savour it.", t.hour, t.minute),
            None => "Locked out. Savour it.".into(),
        };
    }
    match u.worst().unwrap_or(0) {
        85.. => "Nearly spent. How predictable.".into(),
        60.. => format!("You're burning through it, {hon}."),
        _ => "Reserves adequate. For now.".into(),
    }
}

/// Demo answer to "how much Claude do I have left?".
pub fn usage_answer(u: &UsageView, hon: &str) -> String {
    let (Some(s), Some(w)) = (u.session_pct, u.week_pct) else {
        return format!("I have no ledger to read from, {hon}. Someone forgot to sign me in.");
    };
    let reset = u.session_reset_min.map(fmt_minutes).unwrap_or_else(|| "no time at all".into());
    if s >= 85 {
        format!("{s}% gone. The session resets in {reset}. I'd stop talking, {hon}.")
    } else {
        format!("{s}% of this session consumed, {w}% of the week. Reset in {reset}. Spend wisely, {hon}. Or don't.")
    }
}

pub const QUESTION_SUFFIX: &str = ", how much Claude do I have left?";

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wrap_keeps_last_two() {
        let l = wrap_last_two("aaa bbb ccc ddd", 7);
        assert_eq!(l, vec!["aaa bbb", "ccc ddd"]);
        let l = wrap_last_two("one two three four five six", 9);
        assert_eq!(l, vec!["four five", "six"]);
    }

    #[test]
    fn verdicts() {
        let mut u = UsageView { signed_in: true, session_pct: Some(10), week_pct: Some(61), ..UsageView::none() };
        assert_eq!(ledger_verdict(&u, "sir"), "You're burning through it, sir.");
        u.session_pct = Some(90);
        assert_eq!(ledger_verdict(&u, "sir"), "Nearly spent. How predictable.");
        u.week_pct = Some(1);
        u.session_pct = Some(1);
        assert_eq!(ledger_verdict(&u, "sir"), "Reserves adequate. For now.");
        assert!(ledger_verdict(&UsageView::none(), "guv").contains("guv"));
    }
}
