#!/usr/bin/env python3
"""ipcreport - turn ipcbench console output into a Markdown report and an SVG chart.

Usage: python ipcreport.py serial.log [-o outdir]

Reads every "IPCB ..." line (ignores everything else, so a raw QEMU serial log
works), writes <outdir>/report.md and <outdir>/p99.svg, and prints the table.
Standard library only; runs on Windows, Linux and macOS.

SPDX-License-Identifier: Apache-2.0
"""
import argparse
import math
import os
import re
import sys

LOADS = ["none", "low", "same"]
LOAD_LABEL = {"none": "idle", "low": "busy CPUs, lower prio", "same": "busy CPUs, same prio"}
COLORS = {"none": "#2a7ab9", "low": "#3aa36b", "same": "#d9822b"}


def parse(path):
    rows, info, phases = [], {}, {}
    timers = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            m = re.search(r"IPCB(_INFO|_PHASE|_TIMER)?\s+(.*)$", line)
            if not m:
                continue
            kv = dict(re.findall(r"(\w+)=(\S+)", m.group(2)))
            if m.group(1) == "_TIMER":
                timers.append(kv)
                continue
            if m.group(1) == "_INFO":
                info = kv
            elif m.group(1) == "_PHASE":
                phases[kv.get("load")] = float(kv.get("seconds", "nan"))
            elif "test" in kv:
                for k in ("min_us", "p50_us", "p99_us", "p999_us", "max_us", "mean_us"):
                    kv[k] = float(kv[k])
                kv["n"] = int(kv["n"])
                kv["size"] = int(kv["size"])
                rows.append(kv)
    return info, phases, rows, timers


def label(r):
    t, s = r["test"], r["size"]
    if t in ("msg", "msgx"):
        return f"{t} {s} B"
    if t == "timer":
        return f"timer {s} us"
    return t


def timer_health(rows, timers):
    """Flag a coarse clock: a periodic timer whose median interval error is more
    than half its period, or whose achieved pulse rate is >10% below the request."""
    out, warn = [], False
    by = {(r["size"], r["load"]): r for r in rows if r["test"] == "timer"}
    seen = set()
    for t in timers:
        per, load = int(t["period_us"]), t["load"]
        seen.add((per, load))
        r = by.get((per, load))
        rate, exp = float(t["pulse_rate_hz"]), float(t["expected_hz"])
        bad = rate < 0.9 * exp or (r is not None and r["p50_us"] > per / 2)
        warn |= bad
        out.append(f"| {per} us | {load} | {rate:.1f} / {exp:.1f} Hz | {t['overruns']} | "
                   f"{r['p50_us']:.0f} us | {'COARSE CLOCK' if bad else 'ok'} |" if r else "")
    for (per, load), r in by.items():
        if (per, load) not in seen:
            bad = r["p50_us"] > per / 2
            warn |= bad
            out.append(f"| {per} us | {load} | n/a | n/a | {r['p50_us']:.0f} us | {'COARSE CLOCK' if bad else 'ok'} |")
    hdr = ["| timer period | load | achieved / requested rate | overruns | median interval error | verdict |",
           "|---|---|---|---|---|---|"]
    return "\n".join(hdr + [o for o in out if o]), warn


def table(rows):
    keys = []
    for r in rows:
        k = (r["test"], r["size"])
        if k not in keys:
            keys.append(k)
    by = {(r["test"], r["size"], r["load"]): r for r in rows}
    out = ["| test | " + " | ".join(f"{LOAD_LABEL[l]} p50 / p99 / max (us)" for l in LOADS) + " |",
           "|---|" + "---|" * len(LOADS)]
    for t, s in keys:
        cells = []
        for l in LOADS:
            r = by.get((t, s, l))
            cells.append(f"{r['p50_us']:.1f} / {r['p99_us']:.1f} / {r['max_us']:.1f}" if r else "-")
        out.append(f"| {label({'test': t, 'size': s})} | " + " | ".join(cells) + " |")
    return "\n".join(out)


def svg(rows, path):
    pick = [("msg", 64), ("msgx", 64), ("msg", 16384), ("pulse", 0)]
    pick += sorted({("timer", r["size"]) for r in rows if r["test"] == "timer"})
    by = {(r["test"], r["size"], r["load"]): r for r in rows}
    vals = [r["p99_us"] for r in rows if (r["test"], r["size"]) in pick and r["p99_us"] > 0]
    if not vals:
        return False
    lo = 10 ** math.floor(math.log10(min(vals)))
    hi = 10 ** math.ceil(math.log10(max(vals)))
    W, H, L, B, T = 760, 360, 70, 60, 40
    ph = H - B - T
    gw = (W - L - 20) / len(pick)
    bw = gw / (len(LOADS) + 1)

    def y(v):
        return T + ph * (1 - (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)))

    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" font-family="sans-serif" font-size="12">',
         f'<rect width="{W}" height="{H}" fill="white"/>',
         f'<text x="{L}" y="22" font-size="14" font-weight="bold">p99 latency on QNX 8.0 (log scale, microseconds)</text>']
    d = lo
    while d <= hi * 1.0001:
        s.append(f'<line x1="{L}" x2="{W-20}" y1="{y(d):.1f}" y2="{y(d):.1f}" stroke="#ddd"/>')
        s.append(f'<text x="{L-6}" y="{y(d)+4:.1f}" text-anchor="end">{d:g}</text>')
        d *= 10
    for i, (t, sz) in enumerate(pick):
        x0 = L + i * gw + bw / 2
        for j, l in enumerate(LOADS):
            r = by.get((t, sz, l))
            if not r or r["p99_us"] <= 0:
                continue
            yy = y(max(r["p99_us"], lo))
            s.append(f'<rect x="{x0 + j*bw:.1f}" y="{yy:.1f}" width="{bw-2:.1f}" height="{T+ph-yy:.1f}" fill="{COLORS[l]}">'
                     f'<title>{label(r)} {LOAD_LABEL[l]}: p99 {r["p99_us"]:.1f} us</title></rect>')
        s.append(f'<text x="{L + i*gw + gw/2:.1f}" y="{T+ph+18}" text-anchor="middle">{label({"test": t, "size": sz})}</text>')
    lx = L
    for l in LOADS:
        s.append(f'<rect x="{lx}" y="{H-22}" width="12" height="12" fill="{COLORS[l]}"/>')
        s.append(f'<text x="{lx+16}" y="{H-12}">{LOAD_LABEL[l]}</text>')
        lx += 200
    s.append("</svg>")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(s))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log")
    ap.add_argument("-o", "--outdir", default=".")
    a = ap.parse_args()
    info, phases, rows, timers = parse(a.log)
    if not rows:
        print("no IPCB lines found in", a.log, file=sys.stderr)
        return 1
    os.makedirs(a.outdir, exist_ok=True)
    md = ["# ipcbench report", ""]
    if info:
        md.append("Target: " + ", ".join(f"{k}={v}" for k, v in info.items()))
    if phases:
        md.append("Phase durations: " + ", ".join(f"{k} {v:.1f} s" for k, v in phases.items()))
    md += ["", table(rows), "", "![p99](p99.svg)", ""]
    th, warn = timer_health(rows, timers)
    md += ["## Timer health", "", th, ""]
    if warn:
        md += ["**Warning:** the guest clock looks coarse: timers fire late or get folded together. "
               "Under QEMU on Windows this has been seen to depend on the host's timer resolution. "
               "Treat the timer and same-priority numbers from this run as emulator artefacts.", ""]
    with open(os.path.join(a.outdir, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(md))
    svg(rows, os.path.join(a.outdir, "p99.svg"))
    print(table(rows))
    print()
    print(th)
    if warn:
        print("WARNING: coarse guest clock detected (see report.md)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
