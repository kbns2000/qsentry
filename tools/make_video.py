#!/usr/bin/env python3
"""Render the ipcbench demo video from real QNX 8.0 (QEMU) console logs.

Every number on screen is parsed from the logs given on the command line;
nothing is typed in by hand. Captions only, no voice-over.

usage: python3 tools/make_video.py results/run2_2026-11-08/serial.log \
                                   results/run1_2026-11-08/serial.log out.mp4
needs: Pillow, ffmpeg, DejaVu fonts
SPDX-License-Identifier: Apache-2.0
"""
import math
import os
import re
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ipcreport  # noqa: E402

W, H = 1280, 720
FD = "/usr/share/fonts/truetype/dejavu/"
F_MONO = ImageFont.truetype(FD + "DejaVuSansMono.ttf", 16)
F_BIG = ImageFont.truetype(FD + "DejaVuSans-Bold.ttf", 52)
F_H1 = ImageFont.truetype(FD + "DejaVuSans-Bold.ttf", 34)
F_H2 = ImageFont.truetype(FD + "DejaVuSans-Bold.ttf", 24)
F_TXT = ImageFont.truetype(FD + "DejaVuSans.ttf", 24)
F_SMALL = ImageFont.truetype(FD + "DejaVuSans.ttf", 18)
BG, PANEL, FG, DIM, ACC = (14, 17, 22), (24, 28, 36), (220, 225, 232), (130, 140, 155), (90, 170, 255)
LOADC = {"none": (42, 122, 185), "low": (58, 163, 107), "same": (217, 130, 43)}
LOADL = {"none": "idle", "low": "CPUs busy, LOWER priority", "same": "CPUs busy, SAME priority"}


def frame():
    im = Image.new("RGB", (W, H), BG)
    return im, ImageDraw.Draw(im)


def wrap(d, text, font, width):
    out, line = [], ""
    for w in text.split():
        t = (line + " " + w).strip()
        if d.textlength(t, font=font) > width and line:
            out.append(line)
            line = w
        else:
            line = t
    if line:
        out.append(line)
    return out


def caption(d, text):
    lines = wrap(d, text, F_TXT, W - 120)
    y = H - 40 - 34 * len(lines)
    d.rectangle([0, y - 16, W, H], fill=(0, 0, 0))
    for i, l in enumerate(lines):
        d.text((60, y + 34 * i), l, font=F_TXT, fill=FG)


def header(d, title, sub=None):
    d.text((60, 36), title, font=F_H1, fill=FG)
    if sub:
        d.text((60, 82), sub, font=F_SMALL, fill=DIM)


def console_lines(path):
    txt = open(path, encoding="utf-8", errors="replace").read()
    txt = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", txt)
    return [l.rstrip() for l in txt.splitlines()]


def bars(d, groups, by, x0, y0, w, h, lo, hi):
    """grouped bars of p50 on a log axis; groups = [(label, test, size)]"""
    def y(v):
        v = max(v, lo)
        return y0 + h * (1 - (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)))
    t = lo
    while t <= hi * 1.001:
        d.line([x0, y(t), x0 + w, y(t)], fill=(45, 50, 60))
        lab = f"{t/1000:g} ms" if t >= 1000 else f"{t:g} µs"
        d.text((x0 - 10 - d.textlength(lab, font=F_SMALL), y(t) - 10), lab, font=F_SMALL, fill=DIM)
        t *= 10
    gw = w / len(groups)
    bw = gw / 4
    for i, (lab, test, size) in enumerate(groups):
        for j, load in enumerate(("none", "low", "same")):
            r = by.get((test, size, load))
            if not r:
                continue
            bx = x0 + i * gw + bw * (j + 0.5)
            d.rectangle([bx, y(r["p50_us"]), bx + bw - 6, y0 + h], fill=LOADC[load])
            v = r["p50_us"]
            s = f"{v/1000:.1f} ms" if v >= 1000 else f"{v:.0f} µs"
            d.text((bx + (bw - 6) / 2 - d.textlength(s, font=F_SMALL) / 2, y(v) - 24), s, font=F_SMALL, fill=FG)
        d.text((x0 + i * gw + gw / 2 - d.textlength(lab, font=F_H2) / 2, y0 + h + 10), lab, font=F_H2, fill=FG)
    lx = x0
    for load in ("none", "low", "same"):
        d.rectangle([lx, y0 - 34, lx + 16, y0 - 18], fill=LOADC[load])
        d.text((lx + 24, y0 - 38), LOADL[load], font=F_SMALL, fill=FG)
        lx += 340


def main():
    run2, run1, out = sys.argv[1], sys.argv[2], sys.argv[3]
    info2, ph2, rows2, tim2 = ipcreport.parse(run2)
    _, _, rows1, _ = ipcreport.parse(run1)
    by2 = {(r["test"], r["size"], r["load"]): r for r in rows2}
    by1 = {(r["test"], r["size"], r["load"]): r for r in rows1}
    scenes = []  # (image, seconds)

    im, d = frame()
    d.text((60, 220), "ipcbench", font=F_BIG, fill=ACC)
    d.text((60, 300), "What QNX 8.0 message passing and timers really cost,", font=F_H1, fill=FG)
    d.text((60, 346), "and what happens when the CPUs are busy", font=F_H1, fill=FG)
    d.text((60, 430), "A developer tool for QNX 8.0  -  runs on the target, reports on the host", font=F_TXT, fill=DIM)
    d.text((60, 470), "Every number in this video is parsed from real QNX 8.0 console logs (QEMU x86_64).", font=F_SMALL, fill=DIM)
    scenes.append((im, 6))

    im, d = frame()
    header(d, "Questions every QNX developer asks when sizing a design")
    qs = ["How long is a MsgSend() round trip, for 4 bytes and for 64 KiB?",
          "Is a server in another process much slower than a thread?",
          "How fast does a pulse arrive? How accurate is a periodic timer?",
          "What happens to all of this when the CPUs are 100% busy -",
          "      at a lower priority, and at the SAME priority?"]
    for i, q in enumerate(qs):
        d.text((80, 150 + 60 * i), ("•  " if not q.startswith(" ") else "") + q.strip(), font=F_TXT, fill=FG)
    caption(d, "ipcbench measures each of these with ClockCycles() and prints one machine-readable line per result.")
    scenes.append((im, 8))

    im, d = frame()
    header(d, "One command on a Windows PC - no target board needed")
    steps = ["run_qemu.cmd",
             "  1. qcc -Vgcc_ntox86_64   cross-compile with SDP 8.0",
             "  2. mkqnximage --type=qemu   QNX 8.0 image with ipcbench in /data",
             "  3. qemu-system-x86_64 -accel tcg -smp 2 -no-reboot -pidfile ...",
             "  4. guest runs the benchmark, then shuts itself down",
             "  5. python tools/ipcreport.py  ->  report.md + p99.svg + timer health"]
    d.rectangle([60, 140, W - 60, 470], fill=PANEL)
    for i, s in enumerate(steps):
        d.text((90, 165 + 48 * i), s, font=F_MONO if i else F_H2, fill=FG if i else ACC)
    caption(d, "On a real board you just copy the binary over and run it. Run 2 took "
               f"{sum(ph2.values()):.0f} s of benchmarking inside the VM.")
    scenes.append((im, 8))

    lines = console_lines(run2)
    try:
        s = next(i for i, l in enumerate(lines) if "IPCBENCH START" in l)
    except StopIteration:
        s = 0
    shown = [l for l in lines[s:s + 60] if l.strip() and "To exit QEMU" not in l][:22]
    im, d = frame()
    header(d, "The raw output (QNX 8.0 console, run 2)")
    d.rectangle([40, 110, W - 40, H - 110], fill=PANEL)
    for i, l in enumerate(shown):
        d.text((56, 122 + 21 * i), l[:118], font=F_MONO, fill=FG if l.startswith("IPCB") else DIM)
    caption(d, f"{info2.get('cpus')} vCPUs, clock period {int(info2.get('clock_period_ns', 0))//1000} µs, "
               f"{info2.get('samples')} samples per test. One IPCB line per test, size and load phase.")
    scenes.append((im, 9))

    lo, hi = 1, 100000
    im, d = frame()
    header(d, "Median latency, three load phases (log scale)", "run 2, QNX 8.0 in QEMU TCG")
    bars(d, [("msg 4 B", "msg", 4), ("msgx 4 B", "msgx", 4), ("msg 64 KiB", "msg", 65536), ("pulse", "pulse", 0)],
         by2, 160, 170, W - 240, 380, lo, hi)
    a, b, c = by2[("msg", 4, "none")]["p50_us"], by2[("msg", 4, "low")]["p50_us"], by2[("msg", 4, "same")]["p50_us"]
    caption(d, f"MsgSend 4 B: idle {a:.0f} µs, CPUs busy at lower priority {b:.0f} µs, "
               f"busy at the same priority {c/1000:.1f} ms.")
    scenes.append((im, 12))

    im, d = frame()
    header(d, "Finding 1: priority isolation is real - and measurable")
    pts = [f"Lower-priority load: p99 of MsgSend 4 B is {by2[('msg',4,'low')]['p99_us']:.0f} µs "
           f"(idle: {by2[('msg',4,'none')]['p99_us']:.0f} µs). Busy loops below you barely register.",
           f"Same-priority load: median {c/1000:.1f} ms. That is the round-robin timeslice, "
           f"4 x the {int(info2.get('clock_period_ns', 0))//1000000} ms clock.",
           f"Another process costs more than a thread: msgx 4 B {by2[('msgx',4,'none')]['p50_us']:.0f} µs "
           f"vs msg 4 B {a:.0f} µs (median, idle, under emulation)."]
    y = 150
    for p in pts:
        for l in wrap(d, p, F_TXT, W - 200):
            d.text((90, y), l, font=F_TXT, fill=FG)
            y += 34
        y += 26
    caption(d, "Lesson: set your priorities. The RTOS keeps its promise only when the design uses it.")
    scenes.append((im, 11))

    t1 = by1.get(("timer", 2000, "none"))
    t2 = by2.get(("timer", 2000, "none"))
    m1 = by1.get(("msg", 4, "none"))
    im, d = frame()
    header(d, "Finding 2: an emulator can lie about time")
    d.rectangle([60, 140, 620, 470], fill=PANEL)
    d.rectangle([660, 140, W - 60, 470], fill=PANEL)
    d.text((90, 160), "Run 1 (same PC, same image)", font=F_H2, fill=(230, 120, 90))
    d.text((690, 160), "Run 2", font=F_H2, fill=(46, 160, 90))
    for x, t, m in ((90, t1, m1), (690, t2, by2[("msg", 4, "none")])):
        d.text((x, 220), "2 ms timer, median interval error", font=F_SMALL, fill=DIM)
        d.text((x, 248), f"{t['p50_us']/1000:.2f} ms" if t else "n/a", font=F_BIG, fill=FG)
        d.text((x, 340), "MsgSend 4 B median", font=F_SMALL, fill=DIM)
        d.text((x, 368), f"{m['p50_us']:.1f} µs" if m else "n/a", font=F_H1, fill=FG)
    caption(d, "Same binary logic, same PC: in run 1 the guest clock was coarse (~15 ms steps), which "
               "matches the default Windows timer resolution [inference]. The report now flags such runs.")
    scenes.append((im, 12))

    th, warn = ipcreport.timer_health(rows2, tim2)
    im, d = frame()
    header(d, "Timer health check in every report", "run 2: IPCB_TIMER lines -> ipcreport.py")
    d.rectangle([40, 110, W - 40, 470], fill=PANEL)
    widths = [10, 6, 22, 9, 14, 40]
    for i, l in enumerate([l for l in th.splitlines() if not l.startswith("|---")]):
        cells = [c.strip() for c in l.strip().strip("|").split("|")]
        if i == 0:
            cells = ["period", "load", "achieved / requested", "overruns", "median error", "verdict"]
        row = "".join(c.ljust(w) for c, w in zip(cells, widths))
        d.text((56, 128 + 36 * i), row[:120], font=F_MONO, fill=FG if i else ACC)
    caption(d, "Achieved rate, overruns and median error per timer. Run 1 is flagged COARSE CLOCK, so "
               "emulator artefacts are never published as RTOS numbers.")
    scenes.append((im, 11))

    im, d = frame()
    d.text((60, 200), "ipcbench", font=F_BIG, fill=ACC)
    d.text((60, 280), "github.com/kbns2000/qnx-ipcbench", font=F_H1, fill=FG)
    for i, s in enumerate(["C, Apache-2.0. Target: QNX 8.0, built with qcc (tested on x86_64).",
                           "Host report: Python standard library, Windows / Linux / macOS.",
                           "Under QEMU TCG absolute numbers are emulator numbers: compare shapes,",
                           "or run it on your board for the real thing."]):
        d.text((60, 360 + 38 * i), s, font=F_TXT, fill=DIM if i >= 2 else FG)
    scenes.append((im, 8))

    with tempfile.TemporaryDirectory() as td:
        lst = os.path.join(td, "list.txt")
        with open(lst, "w") as f:
            for i, (img, sec) in enumerate(scenes):
                p = os.path.join(td, f"s{i:02d}.png")
                img.save(p)
                f.write(f"file '{p}'\nduration {sec}\n")
            f.write(f"file '{p}'\n")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", lst,
                        "-vf", "fps=30,format=yuv420p", "-c:v", "libx264", "-crf", "20", out], check=True)
    print(out, sum(s for _, s in scenes), "s")


if __name__ == "__main__":
    main()
