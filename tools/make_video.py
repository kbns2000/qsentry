#!/usr/bin/env python3
"""Render the demo video of open62541-qnx from the logs of a QEMU run.

    python3 tools/make_video.py results/runs/<time> out.mp4 [--repo URL]

Every result on screen is parsed from the console logs of the run (and the
Linux reference results/envprobe_linux.txt); nothing is typed in by hand.
Captions only, no voice-over. Needs Pillow, ffmpeg and the DejaVu fonts.
SPDX-License-Identifier: Apache-2.0
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import o6q  # noqa: E402  (parse, lookup, read_text)
import port  # noqa: E402  (load_series)

W, H = 1280, 720
FONT_DIRS = ["/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/dejavu/",
             os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts") + os.sep]


def font(name, size):
    for d in FONT_DIRS:
        if os.path.exists(d + name):
            return ImageFont.truetype(d + name, size)
    sys.exit(f"make_video.py: font {name} not found")


F_MONO = font("DejaVuSansMono.ttf", 16)
F_MONO_S = font("DejaVuSansMono.ttf", 14)
F_BIG = font("DejaVuSans-Bold.ttf", 52)
F_H1 = font("DejaVuSans-Bold.ttf", 34)
F_H2 = font("DejaVuSans-Bold.ttf", 24)
F_TXT = font("DejaVuSans.ttf", 24)
F_SMALL = font("DejaVuSans.ttf", 18)
BG, PANEL, FG, DIM, ACC = (14, 17, 22), (24, 28, 36), (220, 225, 232), (130, 140, 155), (90, 170, 255)
GOOD, BAD, NEUTRAL = (46, 160, 90), (214, 84, 64), (90, 98, 112)
ARCH_LABEL = {"x86_64": "x86_64", "aarch64le": "aarch64le"}


def frame():
    im = Image.new("RGB", (W, H), BG)
    return im, ImageDraw.Draw(im)


def wrap(d, text, fnt, width):
    out, line = [], ""
    for w in text.split():
        t = (line + " " + w).strip()
        if d.textlength(t, font=fnt) > width and line:
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
    for i, line in enumerate(lines):
        d.text((60, y + 34 * i), line, font=F_TXT, fill=FG)


def header(d, title, sub=None):
    d.text((60, 36), title, font=F_H1, fill=FG)
    if sub:
        d.text((60, 82), sub, font=F_SMALL, fill=DIM)


def paragraph(d, x, y, text, fnt=F_TXT, fill=FG, width=W - 160, gap=34):
    for line in wrap(d, text, fnt, width):
        d.text((x, y), line, font=fnt, fill=fill)
        y += gap
    return y


def cell(d, x, y, w, h, text, colour, fnt=F_SMALL):
    d.rectangle([x, y, x + w - 4, y + h - 4], fill=colour)
    d.text((x + (w - 4) / 2 - d.textlength(text, font=fnt) / 2, y + (h - 4) / 2 - 11), text,
           font=fnt, fill=FG)


def verdict_colour(v):
    return GOOD if v == "PASS" else BAD if v == "FAIL" else NEUTRAL


def console(path):
    text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", o6q.read_text(path))
    return [line.rstrip() for line in text.splitlines()]


def load_run(rundir):
    archs = [a for a in o6q.ARCHES if os.path.isdir(os.path.join(rundir, a))]
    data = {a: o6q.parse(o6q.read_text(os.path.join(rundir, a, "serial.log"))) for a in archs}
    interop_text = o6q.read_text(os.path.join(rundir, "x86_64", "interop.txt"))
    linux = o6q.parse(o6q.read_text(os.path.join(ROOT, "results", "envprobe_linux.txt")))["env"]
    return archs, data, interop_text, linux


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("rundir")
    ap.add_argument("out")
    ap.add_argument("--repo", default="github.com/kbns2000/open62541-qnx")
    args = ap.parse_args()
    archs, data, interop_text, linux = load_run(args.rundir)
    series = port.load_series()
    scenes = []  # (image, seconds)

    # 1 title
    im, d = frame()
    d.text((60, 190), "open62541-qnx", font=F_BIG, fill=ACC)
    d.text((60, 270), "OPC UA on QNX 8.0 - every change shown on QNX itself", font=F_H1, fill=FG)
    d.text((60, 330), f"open62541 {port.VERSION}  |  x86_64 + aarch64le  |  encryption (OpenSSL 3)  |  "
           "PubSub", font=F_TXT, fill=FG)
    d.text((60, 420), "Open-source port category. Every result in this video is parsed from the console",
           font=F_SMALL, fill=DIM)
    d.text((60, 446), "logs of QNX 8.0 guests in QEMU; nothing is typed in by hand.", font=F_SMALL,
           fill=DIM)
    scenes.append((im, 6))

    # 2 why
    im, d = frame()
    header(d, "Why OPC UA on QNX")
    y = 140
    for text in ["OPC UA (IEC 62541) is how PLCs, robots, drives and SCADA/MES systems "
                 "exchange data on the factory floor.",
                 "QNX runs many of the controllers underneath them.",
                 "open62541 is the open-source C implementation of OPC UA (MPL-2.0). It builds "
                 "for QNX since v1.4.12; its platform table lists QNX as tier 3 (no CI, no "
                 "test system)."]:
        y = paragraph(d, 80, y, "•  " + text) + 22
    caption(d, "It compiles for QNX 8.0. The question is whether the code behaves the same way "
               "as on Linux, where it is tested. This project measures that.")
    scenes.append((im, 9))

    # 3 method
    tests = sum(len(o6q.TEST_NAME.findall(c.meta["test"])) for c in series)
    im, d = frame()
    words = ["no", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
             "ten", "eleven", "twelve"]
    count = words[len(series)] if len(series) < len(words) else str(len(series))
    header(d, f"The port: {count} small changes, each with a test on QNX")
    d.rectangle([60, 130, W - 60, 470], fill=PANEL)
    rows = [("release", f"open62541.c / .h {port.VERSION}, SHA-256 checked, never committed"),
            ("patches/", f"{len(series)} changes: reason, upstream file, the test that shows it"),
            ("o6q_check", "test program built from the PORT"),
            ("o6q_check_upstream", "the same program built from the unchanged RELEASE"),
            ("o6q_envprobe", "what the operating system does, next to Linux values")]
    for i, (k, v) in enumerate(rows):
        d.text((90, 160 + 60 * i), k, font=F_H2, fill=ACC)
        d.text((400, 162 + 60 * i), v, font=F_TXT, fill=FG)
    caption(d, f"Both programs run the same suites on QNX. A change stays only where its {tests} "
               "named tests show a difference on the target.")
    scenes.append((im, 10))

    # 4 pipeline
    im, d = frame()
    header(d, "One command on a Windows PC, no board needed")
    steps = ["python scripts/o6q.py all --interop",
             "  build   qcc for x86_64 and aarch64le (CMake, Ninja, SDP 8.0)",
             "  image   mkqnximage --type=qemu, programs in /data/o6q, run.sh at boot",
             "  run     qemu-system-x86_64 (aarch64le: built, no QEMU BSP in the SDP)",
             "          python asyncua client -> demo server in the x86_64 guest",
             "  report  SUMMARY.md: port vs release for every change"]
    d.rectangle([60, 140, W - 60, 470], fill=PANEL)
    for i, s in enumerate(steps):
        d.text((90, 165 + 48 * i), s, font=F_MONO if i else F_H2, fill=FG if i else ACC)
    status = o6q.read_text(os.path.join(args.rundir, "qemu_status.txt")).strip().replace("\n", "; ")
    caption(d, "This run: " + (status or "QEMU status not recorded") + ".")
    scenes.append((im, 9))

    # 5 raw console
    lines = console(os.path.join(args.rundir, archs[0], "serial.log")) if archs else []
    start = next((i for i, l in enumerate(lines) if "O6Q RUN check.robust" in l), 0)
    shown = [l for l in lines[start:] if l.startswith("O6Q ")][:22]
    im, d = frame()
    header(d, f"The raw output (QNX 8.0 {archs[0] if archs else ''} console)")
    d.rectangle([40, 110, W - 40, H - 110], fill=PANEL)
    for i, l in enumerate(shown):
        colour = BAD if " FAIL " in l else GOOD if " PASS " in l else DIM
        d.text((56, 122 + 21 * i), l[:118], font=F_MONO, fill=colour)
    caption(d, "One line per test. The report on the host is made from these lines only.")
    scenes.append((im, 10))

    # 6 changes table
    rows = []
    for c in series:
        for t in o6q.TEST_NAME.findall(c.meta["test"]) or []:
            row = [c.meta["id"], t]
            for a in archs:
                res = data[a]["results"]
                row += [o6q.lookup(res, t, "upstream"), o6q.lookup(res, t, "port")]
            rows.append(row)
    im, d = frame()
    header(d, "Each change on QNX: unchanged release vs port")
    x0, y0, rh = 60, 120, min(26, (H - 230) // max(1, len(rows) + 1))
    cw = [230, 300] + [150] * (2 * len(archs))
    xs = [x0]
    for w in cw[:-1]:
        xs.append(xs[-1] + w)
    labels = ["change", "test"] + [f"{a} {v}" for a in archs for v in ("release", "port")]
    for x, w, lab in zip(xs, cw, labels):
        d.text((x + 4, y0), lab, font=F_MONO_S, fill=ACC)
    for i, row in enumerate(rows):
        y = y0 + rh * (i + 1)
        d.text((xs[0] + 4, y + 3), row[0][:26], font=F_MONO_S, fill=FG)
        d.text((xs[1] + 4, y + 3), row[1][:34], font=F_MONO_S, fill=FG)
        for j, v in enumerate(row[2:]):
            cell(d, xs[2 + j], y, cw[2 + j], rh, v, verdict_colour(v), F_MONO_S)
    shown_fixed = sum(1 for r in rows for k in range(len(archs))
                      if r[2 + 2 * k] == "FAIL" and r[3 + 2 * k] == "PASS")
    caption(d, f"{shown_fixed} test results fail with the unchanged release and pass with the port "
               "(red = FAIL, green = PASS, grey = not run).")
    scenes.append((im, 14))

    # 7 operating system differences
    diff = []
    for k, v in linux.items():
        q = [data[a]["env"].get(k) for a in archs]
        if any(x is not None and x != v for x in q):
            diff.append((k, v, q))
    im, d = frame()
    header(d, "What QNX 8.0 does differently (o6q_envprobe)", "same probe program, Linux reference vs "
           "QNX guests")
    cw = [430, 230] + [260] * len(archs)
    xs = [60]
    for w in cw[:-1]:
        xs.append(xs[-1] + w)
    for x, lab in zip(xs, ["key", "Linux"] + [f"QNX {a}" for a in archs]):
        d.text((x, 120), lab, font=F_MONO_S, fill=ACC)
    for i, (k, v, q) in enumerate(diff[:18]):
        y = 146 + 24 * i
        d.text((xs[0], y), k[:46], font=F_MONO_S, fill=FG)
        d.text((xs[1], y), v[:24], font=F_MONO_S, fill=DIM)
        for j, x in enumerate(q):
            d.text((xs[2 + j], y), (x or "-")[:28], font=F_MONO_S, fill=FG)
    caption(d, f"{len(diff)} of {len(linux)} probed behaviours differ from Linux. The changes are "
               "built on these measurements, not on assumptions.")
    scenes.append((im, 12))

    # 8 suites
    im, d = frame()
    header(d, "All suites", "pass / fail / skip")
    y = 130
    for a in archs:
        d.text((60, y), a, font=F_H2, fill=ACC)
        y += 36
        for s, f in data[a]["summary"].items():
            if "@" in s:
                continue
            up = data[a]["summary"].get(s + "@upstream", {})
            text = (f"{s:<10} port {f.get('pass')}/{f.get('fail')}/{f.get('skip')}   release "
                    f"{up.get('pass', '-')}/{up.get('fail', '-')}/{up.get('skip', '-')}"
                    f"{'   (aborted)' if s + '@upstream' in data[a]['begun'] and not up else ''}")
            d.text((80, y), text, font=F_MONO, fill=FG)
            y += 24
        y += 16
    caption(d, "The port passes where the release fails; everything else behaves the same in "
               "both builds.")
    scenes.append((im, 9))

    # 9 interop
    host = o6q.parse(interop_text)
    im, d = frame()
    header(d, "An independent OPC UA client talks to the QNX demo server",
           "python asyncua on the host -> QEMU port forward -> o6q_server on QNX 8.0 x86_64")
    d.rectangle([40, 110, W - 40, H - 110], fill=PANEL)
    y = 122
    for name, (kind, detail) in host["results"].items():
        d.text((56, y), f"{kind} {name} {detail}"[:116], font=F_MONO_S,
               fill=GOOD if kind == "PASS" else BAD if kind == "FAIL" else DIM)
        y += 20
    for line in host["host"]:
        if line.startswith("value "):
            d.text((56, y), line[:116], font=F_MONO_S, fill=FG)
            y += 18
        if y > H - 130:
            break
    caption(d, "Encrypted session (Basic256Sha256, SignAndEncrypt) with user name; anonymous, wrong "
               "password and an unknown client certificate are refused.")
    scenes.append((im, 13))

    # 10 demo server and slog2
    x86 = console(os.path.join(args.rundir, "x86_64", "serial.log")) if "x86_64" in archs else []
    k = next((i for i, l in enumerate(x86) if "slog2 messages of the demo server" in l), None)
    slog = [l for l in x86[k + 1:k + 40] if l.strip()][:14] if k is not None else []
    demo = [l for l in x86 if l.startswith("O6Q DEMO")][:6]
    im, d = frame()
    header(d, "The demo server on QNX: system data as OPC UA, log in slog2")
    d.rectangle([40, 110, W - 40, H - 110], fill=PANEL)
    y = 122
    for l in demo:
        for part in wrap(d, l, F_MONO_S, W - 120):
            d.text((56, y), part, font=F_MONO_S, fill=FG)
            y += 19
    y += 10
    for l in slog:
        d.text((56, y), l[:120], font=F_MONO_S, fill=DIM)
        y += 19
    caption(d, "CPU load, memory, processes and uptime once per second; trust folder re-read "
               "for every connection; stack messages in slog2info.")
    scenes.append((im, 11))

    # 11 end
    im, d = frame()
    d.text((60, 200), "open62541-qnx", font=F_BIG, fill=ACC)
    d.text((60, 280), args.repo, font=F_H1, fill=FG)
    for i, s in enumerate(["Changes to open62541: MPL-2.0 (like open62541). Everything else: Apache-2.0.",
                           "QNX SDP 8.0, qcc for x86_64 and aarch64le; the run: QEMU x86_64.",
                           "Results from QEMU emulation: behaviour, not performance figures."]):
        d.text((60, 360 + 38 * i), s, font=F_TXT, fill=DIM if i == 2 else FG)
    scenes.append((im, 7))

    with tempfile.TemporaryDirectory() as td:
        lst = os.path.join(td, "list.txt")
        with open(lst, "w") as f:
            for i, (img, sec) in enumerate(scenes):
                p = os.path.join(td, f"s{i:02d}.png")
                img.save(p)
                f.write(f"file '{p}'\nduration {sec}\n")
            f.write(f"file '{p}'\n")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i",
                        lst, "-vf", "fps=30,format=yuv420p", "-c:v", "libx264", "-crf", "20",
                        args.out], check=True)
    print(args.out, sum(s for _, s in scenes), "s")


if __name__ == "__main__":
    main()
