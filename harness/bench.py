#!/usr/bin/env python3
"""memslammer benchmark harness.

Runs memory testers on a remote box over ssh and records time-to-first-error,
including runs that end in a crash or reboot. The target is Windows (OpenSSH
with PowerShell) unless MSBENCH_OS=linux (root login, POSIX shell).

  bench.py deploy [--tm5 DIR]          build + upload binaries and scripts
  bench.py state                        show remote state (free RAM, busy testers)
  bench.py timings                      show the DRAM timings the IMC is running (PawnIO)
  bench.py run TESTER [opts] [-- args]  run one tester, append to results/runs.jsonl
                                        (--gate-dimm C and --sample: start from a known
                                        DIMM temperature and log it, DDR5 on Linux)
  bench.py summary                      aggregate results per label/tester

TESTER is one of:
  karhu                  licensed Karhu RAM Test, via harness/karhu_drv.c (Windows)
  tm5[:CONFIG]           TestMem5 v0.13.1 with bin\\CONFIG (default Absolut @ anta777.cfg) (Windows)
  memslammer             our tester; args after -- are passed through
"""
import argparse
import atexit
import datetime as dt
import json
import os
import re
import shlex
import statistics
import subprocess
import sys
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"
TARGET_OS = os.environ.get("MSBENCH_OS", "windows").lower()
if TARGET_OS not in ("windows", "linux"):
    sys.exit(f"MSBENCH_OS must be windows or linux, not {TARGET_OS!r}")
LINUX = TARGET_OS == "linux"
EXE = "" if LINUX else ".exe"
HOST = os.environ.get("MSBENCH_HOST", "cutestation-win")
RDIR = os.environ.get("MSBENCH_DIR", "/root/msbench" if LINUX else r"C:\msbench")
KARHU_DLL = os.environ.get(
    "MSBENCH_KARHU_DLL",
    r"C:\Users\headpats\Documents\_ostools\KarhuSoftware.RAMTest_1.0.0.1\x64\ramtest.dll")
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
       "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3"]
# memslammer: "[   12.345s] ERROR thread ..."; karhu_drv: "ERROR t=12.345 count=N"
ERR_LINE = re.compile(r"^\[\s*([\d.]+)s\] ERROR|^ERROR t=([\d.]+)")
NOISE = re.compile(r"post-quantum|store now, decrypt|openssh\.com/pq", re.I)


def kv(line):
    return dict(m.groups() for m in re.finditer(r"(\w+)=(\S*)", line))


def ps_quote(s):
    return "'" + s.replace("'", "''") + "'"


def rquote(s):
    """Quote one argument for the target's shell."""
    return shlex.quote(s) if LINUX else ps_quote(s)


def rfile(name):
    """Command prefix that runs deployed file NAME in the target's shell."""
    return shlex.quote(f"{RDIR}/{name}") if LINUX else "& " + ps_quote(RDIR + "\\" + name)


def ssh_run(cmd, timeout=60):
    p = subprocess.run(SSH + [HOST, cmd], capture_output=True, text=True, timeout=timeout)
    out = [l for l in (p.stdout + p.stderr).splitlines() if not NOISE.search(l)]
    return p.returncode, out


def remote_state():
    try:
        rc, out = ssh_run(rfile("guard.sh" if LINUX else "guard.ps1"), timeout=40)
    except subprocess.TimeoutExpired:
        return None
    for l in out:
        if l.startswith("STATE "):
            s = kv(l)
            s["busy"] = [b for b in s.get("busy", "").split(",") if b]
            return s
    return None


def remote_timings():
    """Decoded per-channel timings plus raw UMC registers, or {"error": ...}."""
    try:
        rc, out = ssh_run(rfile("memtimings" + EXE) + " --raw", timeout=30)
    except subprocess.TimeoutExpired:
        return {"error": "timeout"}
    t = {}
    for l in out:
        if l.startswith("TEMP "):
            t["tctl"] = float(kv(l).get("tctl", "nan"))
        elif l.startswith(("TIMINGS ", "RAW ")):
            d = kv(l)
            if "error" in d:
                return {"error": d["error"]}
            key = ("raw" if l.startswith("RAW") else "ch") + d.pop("ch", "?")
            t[key] = d
    return t or {"error": "no output: " + " | ".join(out[:3])}


def timings_label(t):
    c = t.get("ch0")
    if not c:
        return ""
    return f"{c['mts']} c{c['cl']} rcd{c['rcdrd']} rp{c['rp']} ras{c['ras']} rfc{c['rfc']}"


DIMM_TEMPS = ('for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = spd5118 ] && '
              'cat $h/temp1_input; done; true')


def remote_dimm_temps():
    """DDR5 DIMM temperatures in degC (spd5118 sensors, Linux); [] if there are none."""
    if not LINUX:
        return []
    try:
        rc, out = ssh_run(DIMM_TEMPS, timeout=20)
    except subprocess.TimeoutExpired:
        return []
    return [int(l) / 1000 for l in out if l.strip().lstrip("-").isdigit()]


def gate_dimm(limit, max_wait):
    """Wait until every DIMM reads <= limit degC. A fixed idle time starts runs
    at different temperatures, depending on how hot the last run left the DIMMs."""
    t0 = time.monotonic()
    while True:
        temps = remote_dimm_temps()
        if not temps:
            sys.exit("--gate-dimm: no DIMM temperature sensors (needs DDR5 and the spd5118 driver)")
        waited = time.monotonic() - t0
        if max(temps) <= limit:
            print(f"   gate: DIMMs {temps} C <= {limit} C after {waited:.0f}s", flush=True)
            return
        if waited >= max_wait:
            print(f"   gate: gave up after {waited:.0f}s at {temps} C", flush=True)
            return
        time.sleep(20)


def sampler_start():
    """Stream sampler.sh (1 Hz temperatures) from the box into a local file, so a
    crash of the box doesn't lose it. Returns (process, path)."""
    d = RESULTS / "samples"
    d.mkdir(parents=True, exist_ok=True)
    path = d / f".tmp-{uuid.uuid4().hex[:8]}.tsv"
    p = subprocess.Popen(SSH + [HOST, f"sh {shlex.quote(RDIR + '/sampler.sh')} /dev/stdout"],
                         stdout=open(path, "w"), stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
    atexit.register(p.terminate)   # also when the harness gives up before the run
    return p, path


def sampler_stop(samp, run_id):
    p, path = samp
    try:
        ssh_run(r"pkill -f '[s]ampler\.sh'", timeout=20)
    except subprocess.TimeoutExpired:
        pass
    p.terminate()
    p.wait()
    final = path.with_name(f"{run_id}.tsv")
    path.rename(final)
    return final


def remote_events(since_utc):
    if LINUX:
        cmd = rfile("events.sh") + " " + shlex.quote(since_utc)
    else:
        cmd = rfile("events.ps1") + f" -SinceUtc {ps_quote(since_utc)}"
    try:
        rc, out = ssh_run(cmd, timeout=60)
    except subprocess.TimeoutExpired:
        return {"error": "timeout"}
    ev = {"lines": [l for l in out if l.startswith("EVENT ")]}
    for l in out:
        if l.startswith("EVENTS "):
            ev.update(kv(l))
    return ev


def cmd_deploy(a):
    subprocess.run(["make", "-C", str(ROOT), "cli", "tools"], check=True)
    if LINUX:
        if a.tm5:
            sys.exit("--tm5 is Windows-only")
        ssh_run(f"mkdir -p {shlex.quote(RDIR)}")
        files = [ROOT / "build/memslammer", ROOT / "build/memtimings",
                 *sorted((ROOT / "harness/remote").glob("*.sh"))]
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes", *map(str, files), f"{HOST}:{RDIR}/"], check=True)
        ssh_run(f"chmod +x {shlex.quote(RDIR)}/*")
        print("deployed to", HOST, RDIR)
        return
    ssh_run(f"New-Item -ItemType Directory -Force {ps_quote(RDIR)} | Out-Null")
    subprocess.run(["make", "-C", str(ROOT), "build/AMDFamily17.bin"], check=True)
    files = [ROOT / "build/memslammer.exe", ROOT / "build/karhu_drv.exe",
             ROOT / "build/memtimings.exe", ROOT / "build/AMDFamily17.bin",
             *sorted((ROOT / "harness/remote").glob("*.ps1"))]
    dest = HOST + ":" + RDIR.replace("\\", "/") + "/"
    subprocess.run(["scp", "-q", "-o", "BatchMode=yes", *map(str, files), dest], check=True)
    if a.tm5:
        src = Path(a.tm5)
        if not (src / "TM5.exe").exists():
            sys.exit(f"{src} has no TM5.exe")
        ssh_run(f"New-Item -ItemType Directory -Force {ps_quote(RDIR + r'\tm5')} | Out-Null")
        subprocess.run(["scp", "-q", "-r", "-o", "BatchMode=yes",
                        str(src / "TM5.exe"), str(src / "TM5.dll"), str(src / "bin"),
                        dest + "tm5/"], check=True)
    print("deployed to", HOST, RDIR)


def cmd_state(a):
    print(json.dumps(remote_state(), indent=2))


def cmd_timings(a):
    t = remote_timings()
    if "error" in t:
        sys.exit(f"timings unavailable: {t['error']}")
    for k, v in t.items():
        if k.startswith("ch"):
            print(k, " ".join(f"{n}={x}" for n, x in v.items()))
    print("label:", timings_label(t))


def build_remote_cmd(a, state):
    t = a.tester
    mb = a.mb or int(int(state["free_mb"]) * 0.9)
    if LINUX and t != "memslammer":
        sys.exit(f"{t} runs on Windows only")
    if t == "karhu":
        threads = a.threads or 0
        th = threads if threads else "$env:NUMBER_OF_PROCESSORS"
        return (f"& {ps_quote(RDIR + r'\karhu_drv.exe')} {ps_quote(KARHU_DLL)} {mb} {th} {a.seconds}"
                + (" nostop" if a.rate else ""),
                {"mb": mb, "threads": threads or "auto"})
    if t == "tm5" or t.startswith("tm5:"):
        if a.rate:
            sys.exit("--rate is not supported for tm5 (it logs one error per test block)")
        cfg = t.split(":", 1)[1] if ":" in t else "Absolut @ anta777.cfg"
        if not cfg.endswith(".cfg"):
            cfg += ".cfg"
        return (f"& {ps_quote(RDIR + r'\run_tm5.ps1')} -Dir {ps_quote(RDIR + r'\tm5')} "
                f"-Config {ps_quote(cfg)} -Seconds {a.seconds}", {"config": cfg})
    if t == "memslammer":
        # The default mode changed from chain to verify. Records from before
        # that with no -M were chain runs, so always record the mode now.
        extra = a.extra if "-M" in a.extra else ["-M", "verify"] + a.extra
        args = ["-m", str(mb), "-d", str(a.seconds)] + ([] if a.rate else ["-s"])
        if a.threads:
            args += ["-t", str(a.threads)]
        args += extra
        return (rfile("memslammer" + EXE) + " " + " ".join(rquote(x) for x in args),
                {"mb": mb, "threads": a.threads or "auto", "args": extra})
    sys.exit(f"unknown tester {t}")


def wait_back(boot_before, limit):
    """After a lost connection: wait for the host, report whether it rebooted."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < limit:
        s = remote_state()
        if s:
            return ("reboot" if s["boot"] != boot_before else "harness_lost"), s
        time.sleep(10)
    return "host_down", None


def run_once(a, idx):
    state = remote_state()
    if state is None:
        sys.exit("remote host unreachable")
    if state["busy"] and not a.force:
        sys.exit(f"refusing to run: already running on {HOST}: {', '.join(state['busy'])} (use --force)")
    if a.sample and not LINUX:
        sys.exit("--sample needs a Linux target")
    if a.gate_dimm is not None:
        gate_dimm(a.gate_dimm, a.gate_wait)
    samp = sampler_start() if a.sample else None
    if a.idle:
        print(f"   idle {a.idle:.0f}s so the DIMMs cool down", flush=True)
        time.sleep(a.idle)
        state = remote_state()
        if state is None:
            sys.exit("remote host unreachable after idle")
        if state["busy"] and not a.force:
            sys.exit(f"something started during idle: {', '.join(state['busy'])}")
    timings = remote_timings()
    dimm_start = remote_dimm_temps()
    label = a.label or timings_label(timings)
    if a.warmup:
        print(f"   warm-up: memslammer chain {a.warmup:.0f}s (not recorded)", flush=True)
        try:
            ssh_run(rfile("memslammer" + EXE) + f" -M chain -d {a.warmup}", timeout=a.warmup + 120)
        except subprocess.TimeoutExpired:
            pass
        state = remote_state()
        if state is None:
            sys.exit("remote host lost during warm-up")
    remote, params = build_remote_cmd(a, state)
    run_id = dt.datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:4]
    start_utc = dt.datetime.now(dt.timezone.utc).isoformat()
    logdir = RESULTS / "logs"
    logdir.mkdir(parents=True, exist_ok=True)
    logf = open(logdir / f"{run_id}.log", "w")
    print(f"== run {run_id} [{idx + 1}/{a.repeat}] {a.tester} label={label!r} {params}", flush=True)
    logf.write(f"# {remote}\n")

    t0 = time.monotonic()
    last_out = 0.0
    result = None
    err_times = []
    p = subprocess.Popen(SSH + [HOST, remote], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1)
    try:
        for line in p.stdout:
            line = line.rstrip("\r\n")
            if NOISE.search(line):
                continue
            el = time.monotonic() - t0
            last_out = el
            logf.write(f"{el:9.2f} {line}\n")
            logf.flush()
            if not a.quiet or line.startswith(("RESULT", "ERROR", "START")) or "rror" in line:
                print(f"  {el:8.1f}s | {line}", flush=True)
            if line.startswith("RESULT "):
                result = kv(line)
            m = ERR_LINE.search(line)
            if m and len(err_times) < 10000:
                err_times.append(float(m.group(1) or m.group(2)))
        p.wait()
    except KeyboardInterrupt:
        p.terminate()
        p.wait()
        result = result or {"outcome": "interrupted"}
    wall = time.monotonic() - t0

    rec = {"id": run_id, "start_utc": start_utc, "os": TARGET_OS, "label": label, "notes": a.notes,
           "tester": a.tester, "params": params, "seconds_limit": a.seconds,
           "wall_s": round(wall, 2), "free_mb_before": int(state["free_mb"]),
           "mem": {"mts": state.get("mts"), "dimms": state.get("dimms")},
           "timings": timings, "warmup_s": a.warmup, "idle_s": a.idle, "rate_mode": a.rate}
    if a.gate_dimm is not None:
        rec["gate_dimm_c"] = a.gate_dimm
    if dimm_start:
        rec["dimm_start_c"] = dimm_start
    if result and "errors" in result:
        errs = int(result["errors"])
        rec["errors"] = errs
        fe = float(result.get("first_error_s", -1))
        rec["first_error_s"] = fe if fe >= 0 else None
        rec["elapsed_s"] = float(result.get("elapsed_s", wall))
        rec["outcome"] = "error" if errs else result.get("outcome", "pass")
        if rec["outcome"] in ("timeout", "completed"):
            rec["outcome"] = "pass"
        if a.rate:
            rec["error_times_s"] = err_times
            rec["errors_per_min"] = round(errs / rec["elapsed_s"] * 60, 3) if rec["elapsed_s"] > 0 else None
    elif result:
        rec["outcome"] = result["outcome"]
    elif p.returncode not in (None, 255):
        # ssh returned normally but the tester printed no RESULT: bad args,
        # missing binary, ... Not a memory failure. (ssh uses 255 for its own errors.)
        rec["outcome"] = "tester_failed"
        rec["exit_code"] = p.returncode
    else:
        # connection lost: crash, BSOD, reboot or ssh failure
        how, _ = wait_back(state["boot"], a.reboot_wait)
        rec["outcome"] = how
        rec["lost_at_s"] = rec["elapsed_s"] = round(last_out, 1)
        if err_times:
            # errors were reported before the loss; the crash isn't the first one
            rec["errors"] = len(err_times)
            rec["first_error_s"] = err_times[0]
            if a.rate:
                rec["error_times_s"] = err_times
        else:
            rec["first_error_s"] = round(last_out, 1)   # approx: last output before loss
    if rec["outcome"] != "host_down":
        dimm_end = remote_dimm_temps()
        if dimm_end:
            rec["dimm_end_c"] = dimm_end
        rec["events"] = remote_events(start_utc)
        rec["tctl_end"] = remote_timings().get("tctl")
    if samp:
        rec["samples"] = "results/samples/" + sampler_stop(samp, run_id).name
    rec["tctl_start"] = timings.get("tctl")
    with open(RESULTS / "runs.jsonl", "a") as f:
        f.write(json.dumps(rec) + "\n")
    ev = rec.get("events", {})
    hw = " ".join(f"{k}={ev[k]}" for k in ("whea", "mce", "oops") if k in ev) or "hw_events=?"
    print(f"== {run_id}: outcome={rec['outcome']} first_error_s={rec.get('first_error_s')} "
          f"{hw} wall={wall:.0f}s", flush=True)
    logf.close()
    return rec


def cmd_run(a):
    for i in range(a.repeat):
        rec = run_once(a, i)
        if rec["outcome"] in ("host_down", "interrupted"):
            break
        if i + 1 < a.repeat and a.cooldown:
            time.sleep(a.cooldown)


def cmd_summary(a):
    path = RESULTS / "runs.jsonl"
    if not path.exists():
        sys.exit("no results yet")
    groups = {}
    for l in path.read_text().splitlines():
        r = json.loads(l)
        if a.label and r.get("label") != a.label:
            continue
        key = (r.get("label"), r["tester"], json.dumps(r.get("params", {}).get("args", "")),
               bool(r.get("rate_mode")), r.get("warmup_s") or 0, r.get("idle_s") or 0,
               r.get("os", "windows"))
        groups.setdefault(key, []).append(r)

    def med(xs, w):
        return f"{statistics.median(xs):{w}.1f}" if xs else f"{'-':>{w}}"

    print(f"{'label':24} {'tester':56} {'n':>3} {'fail':>4} {'median_tte':>10} {'min':>7} {'max':>7} {'err/min':>8}  outcomes")
    for (label, tester, args, rate, warm, idle, tos), rs in sorted(groups.items(), key=lambda x: (str(x[0][0]), x[0][1], x[0][2])):
        fails = [r for r in rs if r["outcome"] in ("error", "reboot", "harness_lost")]
        ttes = [r["first_error_s"] for r in fails if r.get("first_error_s") is not None]
        rates = [r["errors_per_min"] for r in rs if r.get("errors_per_min") is not None]
        outc = {}
        for r in rs:
            outc[r["outcome"]] = outc.get(r["outcome"], 0) + 1
        name = tester + ("" if args in ('""', "[]") else " " + " ".join(json.loads(args)))
        name += (f" [{tos}]" if tos != "windows" else "") + (" [rate]" if rate else "") + (f" [warm {warm:.0f}s]" if warm else "") + (f" [cold {idle:.0f}s]" if idle else "")
        print(f"{str(label)[:24]:24} {name[:56]:56} {len(rs):3} {len(fails):4} {med(ttes, 10)} "
              f"{(f'{min(ttes):7.1f}' if ttes else f'{chr(45):>7}')} {(f'{max(ttes):7.1f}' if ttes else f'{chr(45):>7}')} "
              f"{med(rates, 8)}  {outc}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("deploy")
    d.add_argument("--tm5", help="local folder containing TM5.exe, TM5.dll, bin/")
    sub.add_parser("state")
    sub.add_parser("timings")
    r = sub.add_parser("run")
    r.add_argument("tester")
    r.add_argument("--seconds", type=float, default=1800)
    r.add_argument("--mb", type=int, help="test size (default 90%% of free RAM; TM5 sizes itself)")
    r.add_argument("--threads", type=int)
    r.add_argument("--label", default="", help="describe the memory settings (default: read from the IMC)")
    r.add_argument("--warmup", type=float, default=0, help="run memslammer chain this many seconds first (unrecorded)")
    r.add_argument("--idle", type=float, default=0, help="sleep this many seconds first so the box is cold (cold-start runs)")
    r.add_argument("--gate-dimm", type=float, metavar="C",
                   help="first wait until every DDR5 DIMM reads <= C degC (Linux; before --idle)")
    r.add_argument("--gate-wait", type=float, default=1200, help="give up on --gate-dimm after this many seconds")
    r.add_argument("--sample", action="store_true",
                   help="log temperatures at 1 Hz to results/samples/<run id>.tsv, from before --idle (Linux)")
    r.add_argument("--rate", action="store_true", help="don't stop at the first error; record error times and errors/min")
    r.add_argument("--notes", default="")
    r.add_argument("--repeat", type=int, default=1)
    r.add_argument("--cooldown", type=float, default=15, help="seconds between repeats")
    r.add_argument("--reboot-wait", type=float, default=900)
    r.add_argument("--force", action="store_true", help="run even if another tester is running")
    r.add_argument("-q", "--quiet", action="store_true", help="only print starts/errors/results")
    r.add_argument("extra", nargs="*", help="after --: extra memslammer args")
    s = sub.add_parser("summary")
    s.add_argument("--label")
    a = ap.parse_args()
    {"deploy": cmd_deploy, "state": cmd_state, "timings": cmd_timings, "run": cmd_run, "summary": cmd_summary}[a.cmd](a)


if __name__ == "__main__":
    main()
