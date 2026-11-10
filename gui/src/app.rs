//! The main window: settings on the left, live results on the right.

use std::collections::VecDeque;
use std::time::Duration;

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::chart::AreaChart;
use gpui_kit::component::notification::NotificationType;
use gpui_kit::component::progress::Progress;
use gpui_kit::component::slider::{Slider, SliderEvent, SliderState};
use gpui_kit::component::switch::Switch;
use gpui_kit::component::tab::{Tab, TabBar};
use gpui_kit::component::{
    ActiveTheme, Disableable as _, Icon, IconName, Sizable as _, Theme, ThemeMode, TitleBar, WindowExt as _, h_flex,
    v_flex,
};
use gpui_kit::{prelude::FluentBuilder as _, *};

use crate::core::{self, Config, Status, Sysinfo, Test};
use crate::history::{self, Entry, hms};

const POLL: Duration = Duration::from_millis(250);
const MAX_SAMPLES: usize = 300;
const MAX_ERRORS_SHOWN: usize = 2000;

const DURATIONS: [(&str, f64); 7] =
    [("∞", 0.), ("5m", 300.), ("15m", 900.), ("30m", 1800.), ("1h", 3600.), ("2h", 7200.), ("4h", 14400.)];
const REREADS: [i32; 4] = [2, 20, 200, 1000];
const HEATS: [i32; 4] = [0, 32, 64, 128];
const VERIFY_BLOCKS: [(&str, u64); 3] = [("4 MiB", 4 << 20), ("16 MiB", 16 << 20), ("64 MiB", 64 << 20)];
const CHAIN_BLOCKS: [(&str, u64); 2] = [("4 KiB", 4096), ("64 KiB", 65536)];
const PASSES: [i32; 4] = [1, 2, 4, 8];

#[derive(Clone, Copy, PartialEq)]
enum Panel {
    Threads,
    Errors,
    Bits,
    Log,
    History,
}
const PANELS: [(Panel, &str); 5] = [
    (Panel::Threads, "Threads"),
    (Panel::Errors, "Errors"),
    (Panel::Bits, "Bit map"),
    (Panel::Log, "Log"),
    (Panel::History, "History"),
];

#[derive(Clone)]
struct Sample {
    t: SharedString,
    gbps: f64,
}

struct Settings {
    mode: i32,
    memory_mb: u64,
    threads: i32,
    duration: usize,
    stop_on_error: bool,
    advanced: bool,
    rereads: usize,
    pattern: i32,
    cache: i32,
    order: i32,
    heat: usize,
    verify_block: usize,
    chain_block: usize,
    passes: usize,
    flush: bool,
    yield_cpu: bool,
}

impl Settings {
    fn config(&self) -> Config {
        let mut c = Config::default();
        c.mode = self.mode;
        c.megabytes = self.memory_mb;
        c.threads = self.threads;
        c.seconds = DURATIONS[self.duration].1;
        c.stop_on_error = self.stop_on_error as i32;
        c.yield_cpu = self.yield_cpu as i32;
        if self.mode == core::MODE_VERIFY {
            c.block = VERIFY_BLOCKS[self.verify_block].1;
            c.rereads = REREADS[self.rereads];
            c.pattern = self.pattern;
            c.cache = self.cache;
            c.order = self.order;
            c.heat = HEATS[self.heat];
        } else {
            c.block = CHAIN_BLOCKS[self.chain_block].1;
            c.passes = PASSES[self.passes];
            c.flush = self.flush as i32;
        }
        c
    }
}

struct Run {
    test: Test,
    cfg: Config,
    status: Status,
    next_err: u64,
    errors: VecDeque<core::Error>,
    bits: [u64; 64],
    threads: Vec<core::ThreadStatus>,
    samples: VecDeque<Sample>,
    sample_t: f64,
    sample_bytes: u64,
    gbps: f64,
    log_t: f64,
    log_bytes: u64,
    stopped_by_user: bool,
    finished: bool,
}

pub struct MemslammerApp {
    sys: Sysinfo,
    version: String,
    s: Settings,
    memory: Entity<SliderState>,
    threads: Entity<SliderState>,
    run: Option<Run>,
    log: Vec<SharedString>,
    history: Vec<Entry>,
    panel: Panel,
    dark: bool,
    _subs: Vec<Subscription>,
}

impl MemslammerApp {
    pub fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        let sys = core::sysinfo();
        let default_mb = (sys.avail_mb * 9 / 10).max(256);
        let memory = cx.new(|_| {
            SliderState::new()
                .max(sys.total_mb.max(512) as f32)
                .min(256.)
                .step(256.)
                .default_value(default_mb as f32)
        });
        let threads = cx.new(|_| {
            SliderState::new().max(sys.logical_cpus.max(1) as f32).min(1.).step(1.).default_value(sys.logical_cpus as f32)
        });
        let subs = vec![
            cx.subscribe(&memory, |this, _, ev: &SliderEvent, cx| {
                if let SliderEvent::Change(v) = ev {
                    this.s.memory_mb = v.start() as u64;
                    cx.notify();
                }
            }),
            cx.subscribe(&threads, |this, _, ev: &SliderEvent, cx| {
                if let SliderEvent::Change(v) = ev {
                    this.s.threads = v.start() as i32;
                    cx.notify();
                }
            }),
        ];
        let _ = window;

        cx.spawn(async move |this, cx| {
            loop {
                smol::Timer::after(POLL).await;
                if this.update(cx, |this, cx| this.poll(cx)).is_err() {
                    break;
                }
            }
        })
        .detach();

        let s = Settings {
            mode: core::MODE_VERIFY,
            memory_mb: default_mb,
            threads: sys.logical_cpus,
            duration: 0,
            stop_on_error: true,
            advanced: false,
            rereads: 1,
            pattern: 2,
            cache: 0,
            order: 0,
            heat: 0,
            verify_block: 1,
            chain_block: 0,
            passes: 2,
            flush: true,
            yield_cpu: true,
        };
        let mut log = Vec::new();
        if !sys.avx2 {
            log.push("This CPU (or OS) does not support AVX2, which memslammer needs.".into());
        }
        MemslammerApp {
            version: core::version(),
            sys,
            s,
            memory,
            threads,
            run: None,
            log,
            history: history::load(),
            panel: Panel::Threads,
            dark: true,
            _subs: subs,
        }
    }

    fn running(&self) -> bool {
        self.run.as_ref().is_some_and(|r| !r.finished)
    }

    fn push_log(&mut self, line: impl Into<SharedString>) {
        self.log.push(line.into());
        if self.log.len() > 5000 {
            self.log.drain(..1000);
        }
    }

    fn start(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.running() {
            if let Some(r) = &mut self.run {
                r.stopped_by_user = true;
                r.test.stop();
            }
            return;
        }
        let cfg = self.s.config();
        match Test::start(&cfg) {
            Err(e) => window.push_notification((NotificationType::Error, SharedString::from(e)), cx),
            Ok(test) => {
                let cfg = test.config();
                self.log.clear();
                self.push_log(cfg.describe());
                self.push_log(format!("CLI equivalent: memslammer {}", cfg.to_args()));
                self.run = Some(Run {
                    status: test.status(),
                    threads: vec![core::ThreadStatus::default(); cfg.threads as usize],
                    test,
                    cfg,
                    next_err: 0,
                    errors: VecDeque::new(),
                    bits: [0; 64],
                    samples: VecDeque::new(),
                    sample_t: 0.,
                    sample_bytes: 0,
                    gbps: 0.,
                    log_t: 0.,
                    log_bytes: 0,
                    stopped_by_user: false,
                    finished: false,
                });
                self.panel = Panel::Threads;
            }
        }
        cx.notify();
    }

    fn poll(&mut self, cx: &mut Context<Self>) {
        let Some(r) = &mut self.run else { return };
        if r.finished {
            return;
        }
        r.status = r.test.status();
        let s = r.status;
        let mut lines: Vec<String> = Vec::new();

        for e in r.test.errors_since(r.next_err) {
            r.next_err = e.index + 1;
            let b = e.bits64();
            for i in 0..64 {
                if b >> i & 1 == 1 {
                    r.bits[i] += 1;
                }
            }
            let mut line = format!(
                "[{:9.3}s] ERROR thread {} cycle {} (total {})",
                e.time,
                e.thread,
                e.cycle,
                e.index + 1
            );
            if b != 0 {
                line += &format!(" bits 0x{b:016x}");
            }
            lines.push(line);
            r.errors.push_back(e);
            if r.errors.len() > MAX_ERRORS_SHOWN {
                r.errors.pop_front();
            }
        }
        for (i, t) in r.threads.iter_mut().enumerate() {
            *t = r.test.thread(i as i32);
        }
        if s.state >= core::STATE_RUNNING && s.state <= core::STATE_DONE && s.elapsed - r.sample_t >= 1.0 {
            r.gbps = (s.bytes - r.sample_bytes) as f64 / (s.elapsed - r.sample_t) / 1e9;
            r.sample_t = s.elapsed;
            r.sample_bytes = s.bytes;
            r.samples.push_back(Sample { t: hms(s.elapsed).into(), gbps: r.gbps });
            if r.samples.len() > MAX_SAMPLES {
                r.samples.pop_front();
            }
        }
        if s.state == core::STATE_RUNNING && s.elapsed - r.log_t >= 30. {
            lines.push(format!(
                "[{:9.1}s] cycles {}, coverage {:.1}%, {:.2} GB/s, errors {}",
                s.elapsed,
                s.cycles,
                s.coverage,
                (s.bytes - r.log_bytes) as f64 / (s.elapsed - r.log_t) / 1e9,
                s.errors
            ));
            r.log_t = s.elapsed;
            r.log_bytes = s.bytes;
        }

        let mut notify: Option<(NotificationType, String)> = None;
        if s.state == core::STATE_DONE || s.state == core::STATE_FAILED {
            r.finished = true;
            let result = if s.state == core::STATE_FAILED {
                lines.push(format!("Could not start: {}", s.message()));
                notify = Some((NotificationType::Error, s.message()));
                "ERROR"
            } else if s.errors > 0 {
                let l = format!(
                    "FAIL: {} error{}, first after {:.1} s (ran {}, coverage {:.1}%)",
                    s.errors,
                    if s.errors == 1 { "" } else { "s" },
                    s.first_error,
                    hms(s.elapsed),
                    s.coverage
                );
                notify = Some((NotificationType::Error, l.clone()));
                lines.push(l);
                "FAIL"
            } else {
                let tag = if r.stopped_by_user { "STOPPED" } else { "PASS" };
                let l = format!(
                    "{}: no errors in {} (coverage {:.1}%)",
                    if r.stopped_by_user { "Stopped" } else { "PASS" },
                    hms(s.elapsed),
                    s.coverage
                );
                if !r.stopped_by_user {
                    notify = Some((NotificationType::Success, l.clone()));
                }
                lines.push(l);
                tag
            };
            if s.state == core::STATE_DONE && s.elapsed > 0. {
                let e = Entry {
                    when: history::now_utc(),
                    version: self.version.clone(),
                    result: result.into(),
                    elapsed: s.elapsed,
                    errors: s.errors,
                    first_error: s.first_error,
                    coverage: s.coverage,
                    megabytes: s.megabytes,
                    threads: s.threads,
                    gbps: s.bytes as f64 / s.elapsed / 1e9,
                    args: r.cfg.to_args(),
                };
                history::append(&e);
                self.history.push(e);
            }
        }
        for l in lines {
            self.push_log(l);
        }
        if let Some((kind, msg)) = notify {
            if let Some(w) = cx.windows().first().copied() {
                let _ = w.update(cx, |_, window, cx| window.push_notification((kind, SharedString::from(msg)), cx));
            }
        }
        cx.notify();
    }

    fn report(&self) -> String {
        let mut out = format!("memslammer {}\n", self.version);
        out += &format!(
            "System: {}, {} threads, {} MB RAM\n",
            self.sys.cpu, self.sys.logical_cpus, self.sys.total_mb
        );
        if let Some(r) = &self.run {
            let s = &r.status;
            let verdict = if s.errors > 0 {
                format!("FAIL: {} errors, first after {:.1} s", s.errors, s.first_error)
            } else if r.finished && !r.stopped_by_user {
                "PASS: no errors".into()
            } else {
                "no errors so far".into()
            };
            out += &format!("Result: {verdict}\n");
            out += &format!(
                "Ran {} at {:.1} GB/s average, coverage {:.1}%\n",
                hms(s.elapsed),
                if s.elapsed > 0. { s.bytes as f64 / s.elapsed / 1e9 } else { 0. },
                s.coverage
            );
            out += &format!("Test: {}\n", r.cfg.describe());
            out += &format!("Reproduce: memslammer {}\n", r.cfg.to_args());
        }
        out
    }

    // ------------------------------------------------------------ rendering

    fn segmented(
        &self,
        id: &'static str,
        labels: &[&str],
        selected: usize,
        disabled: bool,
        set: fn(&mut Settings, usize),
        cx: &mut Context<Self>,
    ) -> impl IntoElement {
        let mut bar = TabBar::new(id)
            .segmented()
            .small()
            .selected_index(selected)
            .on_click(cx.listener(move |this, ix: &usize, _, cx| {
                if !this.running() {
                    set(&mut this.s, *ix);
                    cx.notify();
                }
            }));
        for l in labels {
            let _ = disabled;
            bar = bar.child(Tab::new().label(SharedString::from(l.to_string())));
        }
        bar
    }

    fn field(label: &str, hint: Option<String>, body: impl IntoElement, muted: Hsla) -> impl IntoElement {
        v_flex()
            .gap_1p5()
            .child(
                h_flex()
                    .justify_between()
                    .child(div().text_sm().font_weight(FontWeight::MEDIUM).child(label.to_string()))
                    .when_some(hint, |el, h| el.child(div().text_xs().text_color(muted).child(h))),
            )
            .child(body)
    }

    fn render_settings(&mut self, cx: &mut Context<Self>) -> impl IntoElement {
        let busy = self.running();
        let theme = cx.theme().clone();
        let muted = theme.muted_foreground;
        let s = &self.s;
        let pct = if self.sys.avail_mb > 0 { s.memory_mb as f64 * 100. / self.sys.avail_mb as f64 } else { 0. };
        let mem_hint = format!("{} MB · {:.0}% of free", s.memory_mb, pct);
        let verify = s.mode == core::MODE_VERIFY;
        let cfg = s.config();
        let args = match cfg.resolved() {
            Ok(c) => format!("memslammer {}", c.to_args()),
            Err(e) => e,
        };

        let mut panel = v_flex()
            .gap_4()
            .when(busy, |el| el.opacity(0.55))
            .child(Self::field(
                "Mode",
                Some(if verify { "fastest to fail (recommended)".into() } else { "Karhu-style chained transform".into() }),
                self.segmented(
                    "mode",
                    &["Verify", "Chain"],
                    if verify { 0 } else { 1 },
                    busy,
                    |s, i| s.mode = if i == 0 { core::MODE_VERIFY } else { core::MODE_CHAIN },
                    cx,
                ),
                muted,
            ))
            .child(Self::field("Memory", Some(mem_hint), Slider::new(&self.memory).disabled(busy), muted))
            .child(Self::field(
                "Threads",
                Some(format!("{} of {}", s.threads, self.sys.logical_cpus)),
                Slider::new(&self.threads).disabled(busy),
                muted,
            ))
            .child(Self::field(
                "Duration",
                None,
                self.segmented(
                    "duration",
                    &DURATIONS.map(|d| d.0),
                    s.duration,
                    busy,
                    |s, i| s.duration = i,
                    cx,
                ),
                muted,
            ))
            .child(
                h_flex().justify_between().child(div().text_sm().child("Stop on first error")).child(
                    Switch::new("stop-err")
                        .checked(s.stop_on_error)
                        .disabled(busy)
                        .on_click(cx.listener(|this, v: &bool, _, cx| {
                            this.s.stop_on_error = *v;
                            cx.notify();
                        })),
                ),
            )
            .child(
                Button::new("adv")
                    .ghost()
                    .small()
                    .icon(if s.advanced { IconName::ChevronDown } else { IconName::ChevronRight })
                    .label("Advanced")
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.s.advanced = !this.s.advanced;
                        cx.notify();
                    })),
            );

        if s.advanced {
            let adv = if verify {
                v_flex()
                    .gap_3()
                    .child(Self::field(
                        "Re-reads per fill",
                        None,
                        self.segmented("rereads", &["2", "20", "200", "1000"], s.rereads, busy, |s, i| s.rereads = i, cx),
                        muted,
                    ))
                    .child(Self::field(
                        "Pattern",
                        None,
                        self.segmented(
                            "pattern",
                            &["random", "toggle", "mul"],
                            match s.pattern { 2 => 0, 0 => 1, _ => 2 },
                            busy,
                            |s, i| {
                                s.pattern = [2, 0, 1][i];
                                if s.pattern != 2 {
                                    s.order = 0;
                                    s.heat = 0;
                                }
                            },
                            cx,
                        ),
                        muted,
                    ))
                    .child(Self::field(
                        "Cache policy",
                        None,
                        self.segmented(
                            "cache",
                            &["none", "flush", "nta"],
                            match s.cache { 0 => 0, 2 => 1, _ => 2 },
                            busy,
                            |s, i| {
                                s.cache = [0, 2, 1][i];
                                if s.cache == 1 {
                                    s.order = 0;
                                }
                            },
                            cx,
                        ),
                        muted,
                    ))
                    .child(Self::field(
                        "Re-read order",
                        None,
                        self.segmented("order", &["seq", "shuffle"], s.order as usize, busy, |s, i| s.order = i as i32, cx),
                        muted,
                    ))
                    .child(Self::field(
                        "FP heat",
                        None,
                        self.segmented("heat", &["off", "32", "64", "128"], s.heat, busy, |s, i| s.heat = i, cx),
                        muted,
                    ))
                    .child(Self::field(
                        "Block",
                        None,
                        self.segmented(
                            "vblock",
                            &VERIFY_BLOCKS.map(|b| b.0),
                            s.verify_block,
                            busy,
                            |s, i| s.verify_block = i,
                            cx,
                        ),
                        muted,
                    ))
            } else {
                v_flex()
                    .gap_3()
                    .child(Self::field(
                        "Passes",
                        None,
                        self.segmented("passes", &["1", "2", "4", "8"], s.passes, busy, |s, i| s.passes = i, cx),
                        muted,
                    ))
                    .child(Self::field(
                        "Block",
                        None,
                        self.segmented(
                            "cblock",
                            &CHAIN_BLOCKS.map(|b| b.0),
                            s.chain_block,
                            busy,
                            |s, i| s.chain_block = i,
                            cx,
                        ),
                        muted,
                    ))
                    .child(
                        h_flex().justify_between().child(div().text_sm().child("Flush cache lines")).child(
                            Switch::new("flush").checked(s.flush).disabled(busy).on_click(cx.listener(
                                |this, v: &bool, _, cx| {
                                    this.s.flush = *v;
                                    cx.notify();
                                },
                            )),
                        ),
                    )
            };
            panel = panel.child(adv.child(
                h_flex().justify_between().child(div().text_sm().child("Yield after each block")).child(
                    Switch::new("yield").checked(s.yield_cpu).disabled(busy).on_click(cx.listener(
                        |this, v: &bool, _, cx| {
                            this.s.yield_cpu = *v;
                            cx.notify();
                        },
                    )),
                ),
            ));
        }

        let start = if busy {
            Button::new("start").danger().large().w_full().icon(IconName::Square).label("Stop")
        } else {
            Button::new("start").primary().large().w_full().icon(IconName::Play).label("Start test").disabled(!self.sys.avx2)
        };
        v_flex()
            .id("settings")
            .w(px(330.))
            .flex_none()
            .h_full()
            .gap_4()
            .p_4()
            .border_r_1()
            .border_color(theme.border)
            .bg(theme.sidebar)
            .overflow_y_scroll()
            .child(panel)
            .child(div().flex_1())
            .child(
                div()
                    .text_xs()
                    .font_family(theme.mono_font_family.clone())
                    .text_color(theme.muted_foreground)
                    .child(args),
            )
            .child(start.on_click(cx.listener(|this, _, window, cx| this.start(window, cx))))
    }

    fn render_hero(&self, cx: &Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let (title, sub, color, icon) = match &self.run {
            None => (
                "Ready".to_string(),
                "Pick a size and press Start. Any error means the memory overclock is unstable.".to_string(),
                theme.foreground,
                IconName::MemoryStick,
            ),
            Some(r) => {
                let s = &r.status;
                match s.state {
                    core::STATE_STARTING => (
                        "Allocating".into(),
                        format!("Committing {} MB of RAM...", s.megabytes),
                        theme.foreground,
                        IconName::Loader,
                    ),
                    core::STATE_FAILED => ("Could not start".into(), s.message(), theme.danger, IconName::CircleX),
                    _ if s.errors > 0 => (
                        if r.finished { "FAIL".into() } else { "FAILING".into() },
                        format!(
                            "{} error{} · first after {:.1} s",
                            s.errors,
                            if s.errors == 1 { "" } else { "s" },
                            s.first_error
                        ),
                        theme.danger,
                        IconName::TriangleAlert,
                    ),
                    core::STATE_DONE if r.stopped_by_user => (
                        "Stopped".into(),
                        format!("No errors in {}", hms(s.elapsed)),
                        theme.success,
                        IconName::CircleCheck,
                    ),
                    core::STATE_DONE => (
                        "PASS".into(),
                        format!("No errors in {}", hms(s.elapsed)),
                        theme.success,
                        IconName::CircleCheck,
                    ),
                    core::STATE_STOPPING => ("Stopping".into(), "Waiting for the workers...".into(), theme.foreground, IconName::Loader),
                    _ => ("Running".into(), "No errors so far".into(), theme.primary, IconName::MemoryStick),
                }
            }
        };
        let elapsed = self.run.as_ref().map(|r| r.status.elapsed).unwrap_or(0.);
        let limit = self.run.as_ref().map(|r| r.cfg.seconds).unwrap_or(0.);
        h_flex()
            .gap_4()
            .items_center()
            .child(Icon::new(icon).size(px(36.)).text_color(color))
            .child(
                v_flex()
                    .flex_1()
                    .child(div().text_3xl().font_weight(FontWeight::BOLD).text_color(color).child(title))
                    .child(div().text_sm().text_color(theme.muted_foreground).child(sub)),
            )
            .child(
                v_flex()
                    .items_end()
                    .child(
                        div()
                            .text_3xl()
                            .font_family(theme.mono_font_family.clone())
                            .child(hms(elapsed)),
                    )
                    .child(div().text_xs().text_color(theme.muted_foreground).child(if limit > 0. {
                        format!("of {}", hms(limit))
                    } else {
                        "until stopped".into()
                    })),
            )
    }

    fn stat(&self, label: &str, value: String, color: Hsla, cx: &Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        v_flex()
            .flex_1()
            .min_w(px(120.))
            .p_3()
            .gap_1()
            .rounded(theme.radius)
            .border_1()
            .border_color(theme.border)
            .bg(theme.background)
            .child(div().text_xs().text_color(theme.muted_foreground).child(label.to_string()))
            .child(div().text_xl().font_weight(FontWeight::SEMIBOLD).text_color(color).child(value))
    }

    fn render_stats(&self, cx: &Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let (cov, gbps, errs, mem, err_color) = match &self.run {
            Some(r) => (
                format!("{:.1}%", r.status.coverage),
                format!(
                    "{:.1} GB/s",
                    if r.finished && r.status.elapsed > 0. { r.status.bytes as f64 / r.status.elapsed / 1e9 } else { r.gbps }
                ),
                r.status.errors.to_string(),
                format!("{} MB", r.status.megabytes),
                if r.status.errors > 0 { theme.danger } else { theme.success },
            ),
            None => ("-".into(), "-".into(), "-".into(), "-".into(), theme.foreground),
        };
        h_flex()
            .gap_3()
            .flex_wrap()
            .child(self.stat("Coverage", cov, theme.foreground, cx))
            .child(self.stat("Throughput", gbps, theme.foreground, cx))
            .child(self.stat("Errors", errs, err_color, cx))
            .child(self.stat("Tested", mem, theme.foreground, cx))
    }

    fn render_chart(&self, cx: &Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let data: Vec<Sample> = self.run.as_ref().map(|r| r.samples.iter().cloned().collect()).unwrap_or_default();
        let color = if self.run.as_ref().is_some_and(|r| r.status.errors > 0) { theme.danger } else { theme.chart_1 };
        v_flex()
            .h(px(170.))
            .p_3()
            .gap_2()
            .rounded(theme.radius)
            .border_1()
            .border_color(theme.border)
            .child(
                h_flex()
                    .justify_between()
                    .child(div().text_sm().font_weight(FontWeight::MEDIUM).child("Throughput"))
                    .child(div().text_xs().text_color(theme.muted_foreground).child("GB/s, read + write")),
            )
            .child(if data.len() < 2 {
                div()
                    .flex_1()
                    .flex()
                    .items_center()
                    .justify_center()
                    .text_sm()
                    .text_color(theme.muted_foreground)
                    .child("Starts plotting once the test runs")
                    .into_any_element()
            } else {
                AreaChart::new(data)
                    .x(|d| d.t.clone())
                    .y(|d| d.gbps)
                    .stroke(color)
                    .fill(linear_gradient(
                        0.,
                        linear_color_stop(color.opacity(0.35), 1.),
                        linear_color_stop(theme.background.opacity(0.05), 0.),
                    ))
                    .y_axis(true)
                    .y_tick_count(3)
                    .y_tick_format(|v| format!("{v:.0}"))
                    .x_tick_count(4)
                    .into_any_element()
            })
    }

    fn render_threads(&self, cx: &Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let Some(r) = &self.run else { return self.empty("Per-thread progress appears here.", cx).into_any_element() };
        let cells = r.threads.iter().enumerate().map(|(i, t)| {
            let bad = t.errors > 0;
            v_flex()
                .w(px(86.))
                .p_2()
                .gap_1()
                .rounded(theme.radius)
                .border_1()
                .border_color(if bad { theme.danger } else { theme.border })
                .bg(if bad { theme.danger.opacity(0.15) } else { theme.background })
                .child(div().text_xs().text_color(theme.muted_foreground).child(format!("thread {i}")))
                .child(div().text_sm().font_weight(FontWeight::SEMIBOLD).child(format!("{:.0}%", t.coverage)))
                .child(
                    Progress::new(SharedString::from(format!("tp{i}")))
                        .xsmall()
                        .value((t.coverage % 100.) as f32)
                        .when(bad, |p| p.color(theme.danger)),
                )
                .child(
                    div()
                        .text_xs()
                        .text_color(if bad { theme.danger } else { theme.muted_foreground })
                        .child(if bad { format!("{} err", t.errors) } else { format!("{} cyc", t.cycles) }),
                )
        });
        h_flex().flex_wrap().gap_2().children(cells).into_any_element()
    }

    fn render_errors(&self, cx: &Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let Some(r) = self.run.as_ref().filter(|r| !r.errors.is_empty()) else {
            return self.empty("No errors. Each error is listed here with its time, thread and failing bits.", cx).into_any_element();
        };
        let mono = theme.mono_font_family.clone();
        let header = h_flex()
            .gap_4()
            .text_xs()
            .text_color(theme.muted_foreground)
            .child(div().w(px(90.)).child("time"))
            .child(div().w(px(60.)).child("thread"))
            .child(div().w(px(60.)).child("cycle"))
            .child(div().w(px(110.)).child("block at"))
            .child(div().child("bits (64-bit word)"));
        let rows = r.errors.iter().rev().take(500).map(|e| {
            h_flex()
                .gap_4()
                .text_sm()
                .font_family(mono.clone())
                .child(div().w(px(90.)).child(format!("{:.3} s", e.time)))
                .child(div().w(px(60.)).child(e.thread.to_string()))
                .child(div().w(px(60.)).child(e.cycle.to_string()))
                .child(div().w(px(110.)).child(format!("{} MiB", e.offset >> 20)))
                .child(div().text_color(theme.danger).child(if e.bits64() != 0 {
                    format!("{:016x}", e.bits64())
                } else {
                    "checksum".into()
                }))
        });
        v_flex().gap_1().child(header).children(rows).into_any_element()
    }

    fn render_bits(&self, cx: &Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let bits = self.run.as_ref().map(|r| r.bits).unwrap_or([0; 64]);
        let max = bits.iter().copied().max().unwrap_or(0).max(1);
        let rows = (0..8).rev().map(|lane| {
            h_flex()
                .gap_1()
                .items_center()
                .child(div().w(px(56.)).text_xs().text_color(theme.muted_foreground).child(format!("byte {lane}")))
                .children((0..8).rev().map(move |bit| {
                    let i = lane * 8 + bit;
                    let n = bits[i];
                    let a = if n == 0 { 0. } else { 0.25 + 0.75 * n as f32 / max as f32 };
                    div()
                        .size(px(30.))
                        .rounded(px(4.))
                        .border_1()
                        .border_color(theme.border)
                        .bg(if n == 0 { theme.background } else { theme.danger.opacity(a) })
                        .flex()
                        .items_center()
                        .justify_center()
                        .text_xs()
                        .child(if n == 0 { String::new() } else { n.to_string() })
                }))
        });
        v_flex()
            .gap_1()
            .children(rows)
            .child(
                div().pt_3().text_xs().text_color(theme.muted_foreground).child(
                    "Which bit positions of a 64-bit word flipped, counted per error (verify mode). On DDR4 a byte \
                     lane is one DQ group (one chip on an x8 DIMM); channel interleaving hides which DIMM. Timing \
                     faults often hit the same weakest DQ line every time: on our test box, tCL/tRCD set too low \
                     flipped only byte 1 bit 7.",
                ),
            )
    }

    fn render_log(&self, cx: &Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        if self.log.is_empty() {
            return self.empty("The run log (same lines as the CLI) appears here.", cx).into_any_element();
        }
        v_flex()
            .gap_0p5()
            .text_xs()
            .font_family(theme.mono_font_family.clone())
            .children(self.log.iter().rev().take(1000).rev().cloned().map(|l| div().child(l)))
            .into_any_element()
    }

    fn render_history(&self, cx: &Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        if self.history.is_empty() {
            return self.empty("Finished runs are kept here, newest first.", cx).into_any_element();
        }
        let mono = theme.mono_font_family.clone();
        let rows = self.history.iter().rev().take(200).map(|e| {
            let color = match e.result.as_str() {
                "FAIL" => theme.danger,
                "PASS" => theme.success,
                _ => theme.muted_foreground,
            };
            h_flex()
                .gap_4()
                .text_sm()
                .child(div().w(px(130.)).font_family(mono.clone()).child(e.when.clone()))
                .child(div().w(px(70.)).font_weight(FontWeight::SEMIBOLD).text_color(color).child(e.result.clone()))
                .child(div().w(px(80.)).font_family(mono.clone()).child(hms(e.elapsed)))
                .child(div().w(px(110.)).child(if e.errors > 0 {
                    format!("{} err @ {:.0}s", e.errors, e.first_error)
                } else {
                    "0 err".into()
                }))
                .child(div().w(px(80.)).child(format!("{:.0}%", e.coverage)))
                .child(div().text_xs().text_color(theme.muted_foreground).font_family(mono.clone()).child(e.args.clone()))
        });
        v_flex().gap_1().children(rows).into_any_element()
    }

    fn empty(&self, text: &str, cx: &Context<Self>) -> impl IntoElement {
        div().p_6().text_sm().text_color(cx.theme().muted_foreground).child(text.to_string())
    }

    fn render_main(&mut self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let progress = self.run.as_ref().map(|r| {
            let running = !r.finished;
            if r.cfg.seconds > 0. {
                Progress::new("progress").value((r.status.elapsed / r.cfg.seconds * 100.).min(100.) as f32)
            } else {
                Progress::new("progress").loading(running && r.status.state == core::STATE_RUNNING).value(0.)
            }
            .small()
            .when(r.status.errors > 0, |p| p.color(theme.danger))
        });
        let panel_ix = PANELS.iter().position(|p| p.0 == self.panel).unwrap_or(0);
        let mut tabs = TabBar::new("panels").underline().small().selected_index(panel_ix).on_click(cx.listener(
            |this, ix: &usize, _, cx| {
                this.panel = PANELS[*ix].0;
                cx.notify();
            },
        ));
        for (p, name) in PANELS {
            let label = match (p, &self.run) {
                (Panel::Errors, Some(r)) if r.status.errors > 0 => format!("{name} ({})", r.status.errors),
                _ => name.to_string(),
            };
            tabs = tabs.child(Tab::new().label(SharedString::from(label)));
        }
        let body = match self.panel {
            Panel::Threads => self.render_threads(cx).into_any_element(),
            Panel::Errors => self.render_errors(cx).into_any_element(),
            Panel::Bits => self.render_bits(cx).into_any_element(),
            Panel::Log => self.render_log(cx).into_any_element(),
            Panel::History => self.render_history(cx).into_any_element(),
        };
        v_flex()
            .flex_1()
            .min_w_0()
            .h_full()
            .p_5()
            .gap_4()
            .child(self.render_hero(cx))
            .when_some(progress, |el, p| el.child(p))
            .child(self.render_stats(cx))
            .child(self.render_chart(cx))
            .child(
                v_flex()
                    .flex_1()
                    .min_h_0()
                    .gap_2()
                    .child(tabs)
                    .child(div().id("panel-body").flex_1().min_h_0().overflow_y_scroll().pt_2().child(body)),
            )
    }

    fn render_title(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        TitleBar::new()
            .child(
                h_flex()
                    .gap_2()
                    .items_center()
                    .child(Icon::new(IconName::MemoryStick).small())
                    .child(div().text_sm().font_weight(FontWeight::SEMIBOLD).child("memslammer"))
                    .child(div().text_xs().text_color(theme.muted_foreground).child(self.version.clone())),
            )
            .child(
                h_flex()
                    .gap_3()
                    .mr_2()
                    .items_center()
                    .child(div().text_xs().text_color(theme.muted_foreground).child(format!(
                        "{} · {} threads · {:.1} GB",
                        self.sys.cpu,
                        self.sys.logical_cpus,
                        self.sys.total_mb as f64 / 1024.
                    )))
                    .child(
                        Button::new("copy-report")
                            .ghost()
                            .xsmall()
                            .icon(IconName::Copy)
                            .tooltip("Copy a text report of this run")
                            .on_click(cx.listener(|this, _, window, cx| {
                                cx.write_to_clipboard(ClipboardItem::new_string(this.report()));
                                window.push_notification((NotificationType::Info, "Report copied"), cx);
                            })),
                    )
                    .child(
                        Button::new("theme")
                            .ghost()
                            .xsmall()
                            .icon(if self.dark { IconName::Sun } else { IconName::Moon })
                            .on_click(cx.listener(|this, _, window, cx| {
                                this.dark = !this.dark;
                                Theme::change(if this.dark { ThemeMode::Dark } else { ThemeMode::Light }, Some(window), cx);
                                cx.notify();
                            })),
                    ),
            )
    }
}

impl Render for MemslammerApp {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let bg = cx.theme().background;
        v_flex()
            .size_full()
            .bg(bg)
            .child(self.render_title(cx))
            .child(h_flex().flex_1().min_h_0().child(self.render_settings(cx)).child(self.render_main(cx)))
    }
}
