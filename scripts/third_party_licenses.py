#!/usr/bin/env python3
"""Write the license notices of every Rust crate linked into memslammer-gui.

  scripts/third_party_licenses.py TARGET_TRIPLE > THIRD-PARTY-LICENSES.txt

Walks the normal (non-build, non-dev) dependency graph for the target from
`cargo metadata` and copies each crate's LICENSE / COPYING / NOTICE files.
Identical texts are printed once, with every crate that uses them.
"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NAMES = ("license", "licence", "copying", "notice", "copyright")


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else None
    cmd = ["cargo", "metadata", "--format-version", "1", "--locked",
           "--manifest-path", str(ROOT / "gui" / "Cargo.toml")]
    if target:
        cmd += ["--filter-platform", target]
    meta = json.loads(subprocess.check_output(cmd))
    pkgs = {p["id"]: p for p in meta["packages"]}
    nodes = {n["id"]: n for n in meta["resolve"]["nodes"]}
    root = meta["resolve"]["root"]

    # crates reachable through normal dependencies only
    seen, todo = set(), [root]
    while todo:
        pid = todo.pop()
        if pid in seen:
            continue
        seen.add(pid)
        for dep in nodes[pid]["deps"]:
            if any(k["kind"] is None for k in dep["dep_kinds"]):
                todo.append(dep["pkg"])
    seen.discard(root)

    texts = {}  # text -> [crate names]
    missing = []
    for pid in sorted(seen, key=lambda i: (pkgs[i]["name"], pkgs[i]["version"])):
        p = pkgs[pid]
        label = f'{p["name"]} {p["version"]} ({p.get("license") or "see text"})'
        d = Path(p["manifest_path"]).parent
        files = sorted(f for f in d.iterdir()
                       if f.is_file() and f.name.lower().startswith(NAMES))
        if not files:
            missing.append(label)
        for f in files:
            t = f.read_text(errors="replace").strip()
            texts.setdefault(t, []).append(label)

    out = sys.stdout
    out.write("memslammer-gui includes the following open-source components.\n")
    out.write(f"{len(seen)} crates; their license texts follow.\n\n")
    for t, users in texts.items():
        out.write("=" * 78 + "\n")
        out.write("Used by: " + ", ".join(sorted(set(users))) + "\n\n")
        out.write(t + "\n\n")
    if missing:
        out.write("=" * 78 + "\n")
        out.write("These crates ship no license file; their license is the SPDX expression shown:\n")
        out.write("\n".join(missing) + "\n")


if __name__ == "__main__":
    main()
