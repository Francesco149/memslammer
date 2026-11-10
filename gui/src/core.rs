//! Safe bindings to the C core (core/memslammer.h). The layouts below must
//! match the C structs exactly.

use std::ffi::{CStr, CString, c_char, c_int};
use std::ptr::NonNull;

pub const MODE_CHAIN: i32 = 0;
pub const MODE_VERIFY: i32 = 1;

pub const STATE_STARTING: i32 = 0;
pub const STATE_RUNNING: i32 = 1;
pub const STATE_STOPPING: i32 = 2;
pub const STATE_DONE: i32 = 3;
pub const STATE_FAILED: i32 = 4;

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Config {
    pub megabytes: u64,
    pub threads: c_int,
    pub mode: c_int,
    pub block: u64,
    pub seconds: f64,
    pub stop_on_error: c_int,
    pub yield_cpu: c_int,
    pub passes: c_int,
    pub flush: c_int,
    pub ops: c_int,
    pub rereads: c_int,
    pub pattern: c_int,
    pub cache: c_int,
    pub order: c_int,
    pub heat: c_int,
    pub late: c_int,
    pub nt_fill: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Status {
    pub state: c_int,
    pub elapsed: f64,
    pub first_error: f64,
    pub errors: u64,
    pub cycles: u64,
    pub bytes: u64,
    pub coverage: f64,
    pub megabytes: u64,
    pub threads: c_int,
    message: [c_char; 160],
}

impl Status {
    pub fn message(&self) -> String {
        unsafe { CStr::from_ptr(self.message.as_ptr()) }.to_string_lossy().into_owned()
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Error {
    pub index: u64,
    pub time: f64,
    pub thread: c_int,
    pub cycle: u64,
    pub offset: u64,
    pub bits: [u64; 4],
    /// verify: which re-read of the block found it (0 = first), -2 = the late
    /// check before the block's next fill; -1 in chain mode
    pub pass: c_int,
    /// verify: how many of 2 flushed re-reads right after failed again; -1 in chain mode
    pub recheck: c_int,
}

impl Error {
    /// Bit positions within a 64-bit word that flipped.
    pub fn bits64(&self) -> u64 {
        self.bits[0] | self.bits[1] | self.bits[2] | self.bits[3]
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ThreadStatus {
    pub cycles: u64,
    pub errors: u64,
    pub coverage: f64,
}

#[repr(C)]
struct RawSysinfo {
    cpu: [c_char; 64],
    logical_cpus: c_int,
    total_mb: u64,
    avail_mb: u64,
    avx2: c_int,
}

#[derive(Clone, Debug)]
pub struct Sysinfo {
    pub cpu: String,
    pub logical_cpus: i32,
    pub total_mb: u64,
    pub avail_mb: u64,
    pub avx2: bool,
}

#[repr(C)]
struct RawTest {
    _private: [u8; 0],
}

unsafe extern "C" {
    fn ms_version() -> *const c_char;
    fn ms_config_init(c: *mut Config);
    #[allow(dead_code)]
    fn ms_config_parse(c: *mut Config, argc: c_int, argv: *mut *mut c_char, err: *mut c_char, n: usize) -> c_int;
    fn ms_config_resolve(c: *mut Config, err: *mut c_char, n: usize) -> c_int;
    fn ms_config_describe(c: *const Config, buf: *mut c_char, n: usize);
    fn ms_config_to_args(c: *const Config, buf: *mut c_char, n: usize);
    fn ms_start(c: *const Config, err: *mut c_char, n: usize) -> *mut RawTest;
    fn ms_stop(t: *mut RawTest);
    fn ms_free(t: *mut RawTest);
    fn ms_get_status(t: *const RawTest, s: *mut Status);
    fn ms_get_thread(t: *const RawTest, thread: c_int, out: *mut ThreadStatus) -> c_int;
    fn ms_get_errors(t: *const RawTest, first: u64, out: *mut Error, max: usize) -> usize;
    fn ms_get_config(t: *const RawTest, c: *mut Config);
    fn ms_get_sysinfo(s: *mut RawSysinfo);
}

fn buf_to_string(buf: &[c_char]) -> String {
    unsafe { CStr::from_ptr(buf.as_ptr()) }.to_string_lossy().into_owned()
}

pub fn version() -> String {
    unsafe { CStr::from_ptr(ms_version()) }.to_string_lossy().into_owned()
}

pub fn sysinfo() -> Sysinfo {
    let mut s: RawSysinfo = unsafe { std::mem::zeroed() };
    unsafe { ms_get_sysinfo(&mut s) };
    Sysinfo {
        cpu: buf_to_string(&s.cpu),
        logical_cpus: s.logical_cpus,
        total_mb: s.total_mb,
        avail_mb: s.avail_mb,
        avx2: s.avx2 != 0,
    }
}

impl Default for Config {
    fn default() -> Self {
        let mut c: Config = unsafe { std::mem::zeroed() };
        unsafe { ms_config_init(&mut c) };
        c
    }
}

impl Config {
    /// Apply CLI-style flags on top of this config.
    #[allow(dead_code)]
    pub fn parse_args(&mut self, args: &str) -> Result<(), String> {
        let owned: Vec<CString> = args
            .split_whitespace()
            .map(|a| CString::new(a).map_err(|_| "bad character in options".to_string()))
            .collect::<Result<_, _>>()?;
        let mut ptrs: Vec<*mut c_char> = owned.iter().map(|s| s.as_ptr() as *mut c_char).collect();
        let mut err = [0 as c_char; 256];
        let r = unsafe { ms_config_parse(self, ptrs.len() as c_int, ptrs.as_mut_ptr(), err.as_mut_ptr(), err.len()) };
        match r {
            0 => Ok(()),
            1 => Err("-h is not an option here".into()),
            _ => Err(buf_to_string(&err)),
        }
    }

    /// Fill in automatic values and validate.
    pub fn resolved(&self) -> Result<Config, String> {
        let mut c = *self;
        let mut err = [0 as c_char; 256];
        if unsafe { ms_config_resolve(&mut c, err.as_mut_ptr(), err.len()) } == 0 {
            Ok(c)
        } else {
            Err(buf_to_string(&err))
        }
    }

    pub fn describe(&self) -> String {
        let mut buf = [0 as c_char; 400];
        unsafe { ms_config_describe(self, buf.as_mut_ptr(), buf.len()) };
        buf_to_string(&buf)
    }

    pub fn to_args(&self) -> String {
        let mut buf = [0 as c_char; 400];
        unsafe { ms_config_to_args(self, buf.as_mut_ptr(), buf.len()) };
        buf_to_string(&buf)
    }
}

/// A running (or finished) test. Dropping it stops the test and waits for
/// the worker threads.
pub struct Test(NonNull<RawTest>);

// The core is thread-safe: every function taking a test may be called from
// any thread.
unsafe impl Send for Test {}
unsafe impl Sync for Test {}

impl Test {
    pub fn start(c: &Config) -> Result<Test, String> {
        let mut err = [0 as c_char; 256];
        let t = unsafe { ms_start(c, err.as_mut_ptr(), err.len()) };
        NonNull::new(t).map(Test).ok_or_else(|| buf_to_string(&err))
    }

    pub fn stop(&self) {
        unsafe { ms_stop(self.0.as_ptr()) }
    }

    pub fn status(&self) -> Status {
        let mut s: Status = unsafe { std::mem::zeroed() };
        unsafe { ms_get_status(self.0.as_ptr(), &mut s) };
        s
    }

    pub fn config(&self) -> Config {
        let mut c: Config = unsafe { std::mem::zeroed() };
        unsafe { ms_get_config(self.0.as_ptr(), &mut c) };
        c
    }

    pub fn thread(&self, i: i32) -> ThreadStatus {
        let mut t = ThreadStatus::default();
        unsafe { ms_get_thread(self.0.as_ptr(), i, &mut t) };
        t
    }

    /// Errors with index >= first that are still kept, oldest first.
    pub fn errors_since(&self, first: u64) -> Vec<Error> {
        let mut out = Vec::new();
        let mut next = first;
        loop {
            let mut chunk: [Error; 64] = unsafe { std::mem::zeroed() };
            let n = unsafe { ms_get_errors(self.0.as_ptr(), next, chunk.as_mut_ptr(), chunk.len()) };
            if n == 0 {
                break;
            }
            out.extend_from_slice(&chunk[..n]);
            next = chunk[n - 1].index + 1;
        }
        out
    }
}

impl Drop for Test {
    fn drop(&mut self) {
        unsafe { ms_free(self.0.as_ptr()) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layout_matches_c() {
        assert_eq!(std::mem::size_of::<Config>(), 72);
        assert_eq!(std::mem::size_of::<Error>(), 72);
    }

    #[test]
    fn parse_and_args() {
        let mut c = Config::default();
        assert_eq!(c.mode, MODE_VERIFY);
        c.parse_args("-M chain -m 1024 -t 2 -F").unwrap();
        assert_eq!(c.to_args(), "-M chain -m 1024 -t 2 -F");
        assert!(c.parse_args("-q").is_err());
    }
}
