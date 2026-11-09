//! Run history (a small TSV file in the user's data directory) and the
//! plain-text report copied for forum posts.

use std::fs;
use std::io::Write;
use std::path::PathBuf;
use std::time::{SystemTime, UNIX_EPOCH};

#[derive(Clone, Debug)]
pub struct Entry {
    pub when: String, // UTC, "2026-11-09 03:12"
    pub version: String,
    pub result: String, // PASS / FAIL / STOPPED / ERROR
    pub elapsed: f64,
    pub errors: u64,
    pub first_error: f64,
    pub coverage: f64,
    pub megabytes: u64,
    pub threads: i32,
    pub gbps: f64,
    pub args: String,
}

fn path() -> Option<PathBuf> {
    let base = if cfg!(windows) {
        std::env::var_os("APPDATA").map(PathBuf::from)
    } else {
        std::env::var_os("XDG_DATA_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")))
    }?;
    Some(base.join("memslammer").join("history.tsv"))
}

pub fn load() -> Vec<Entry> {
    let Some(p) = path() else { return Vec::new() };
    let Ok(text) = fs::read_to_string(p) else { return Vec::new() };
    text.lines()
        .filter_map(|l| {
            let f: Vec<&str> = l.split('\t').collect();
            if f.len() < 11 {
                return None;
            }
            Some(Entry {
                when: f[0].into(),
                version: f[1].into(),
                result: f[2].into(),
                elapsed: f[3].parse().ok()?,
                errors: f[4].parse().ok()?,
                first_error: f[5].parse().ok()?,
                coverage: f[6].parse().ok()?,
                megabytes: f[7].parse().ok()?,
                threads: f[8].parse().ok()?,
                gbps: f[9].parse().ok()?,
                args: f[10].into(),
            })
        })
        .collect()
}

pub fn append(e: &Entry) {
    let Some(p) = path() else { return };
    if let Some(dir) = p.parent() {
        let _ = fs::create_dir_all(dir);
    }
    if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(p) {
        let _ = writeln!(
            f,
            "{}\t{}\t{}\t{:.1}\t{}\t{:.3}\t{:.1}\t{}\t{}\t{:.2}\t{}",
            e.when, e.version, e.result, e.elapsed, e.errors, e.first_error, e.coverage,
            e.megabytes, e.threads, e.gbps, e.args.replace('\t', " ")
        );
    }
}

/// Current UTC time as "YYYY-MM-DD HH:MM".
pub fn now_utc() -> String {
    let secs = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) as i64;
    let (days, rem) = (secs.div_euclid(86400), secs.rem_euclid(86400));
    // civil-from-days (Howard Hinnant)
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + if m <= 2 { 1 } else { 0 };
    format!("{y:04}-{m:02}-{d:02} {:02}:{:02}", rem / 3600, rem / 60 % 60)
}

pub fn hms(s: f64) -> String {
    let t = s.max(0.0) as u64;
    format!("{}:{:02}:{:02}", t / 3600, t / 60 % 60, t % 60)
}
