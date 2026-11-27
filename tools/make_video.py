#!/usr/bin/env python3
"""Render the demo video of qvcan from the logs of one QEMU run.

    python3 tools/make_video.py results/runs/<time> out.mp4 [--repo URL]

Every result on screen is parsed from the console log of the QNX 8.0 guest
(serial.log) and from the host end of the bridge (host_bridge.txt); nothing
is typed in by hand. Captions only, no voice-over, no music.
Needs Pillow, ffmpeg and the DejaVu fonts.
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
import qv  # noqa: E402  (parse, read_text)

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
F_BIG = font("DejaVuSans-Bold.ttf", 56)
F_H1 = font("DejaVuSans-Bold.ttf", 32)
F_H2 = font("DejaVuSans-Bold.ttf", 22)
F_TXT = font("DejaVuSans.ttf", 23)
F_SMALL = font("DejaVuSans.ttf", 17)
BG, PANEL, FG, DIM, ACC = (14, 17, 22), (24, 28, 36), (220, 225, 232), (130, 140, 155), (90, 170, 255)
GOOD, BAD, WARN = (46, 160, 90), (214, 84, 64), (222, 170, 60)
ID_COLOURS = [(90, 170, 255), (46, 180, 140), (222, 170, 60), (180, 120, 230), (230, 120, 160)]

DUMP_FRAME = re.compile(r"QV DUMP\s+([\d.]+)\s+([0-9A-F]+)\s+\[(\d)\]\s+(.*?)\s+bits=(\d+) node=(\d+)")
DUMP_ERR = re.compile(r"QV DUMP\s+([\d.]+)\s+ERROR\s+(.*?)\s+node=(\d+) tec=(\d+) state=(\w+)")
LAT = re.compile(r"(\S+) n=(\d+) p50=([\d.]+)us p90=([\d.]+)us p99=([\d.]+)us")


def frame():
    im = Image.new("RGB", (W, H), BG)
    return im, ImageDraw.Draw(im)


def wrap(d, text, fnt, width):
    out, line = [], ""
    for w in text.split():
        t = (line + " " + w).strip()
        if d.textlength(t, font=fnt) > width and line:
            out.append(line)
            t = w
        line = t
    if line:
        out.append(line)
    return out


def header(d, title, sub=None):
    d.text((60, 34), title, font=F_H1, fill=FG)
    if sub:
        d.text((60, 78), sub, font=F_SMALL, fill=DIM)


def caption(d, text):
    lines = wrap(d, text, F_TXT, W - 120)
    y = H - 30 - 32 * len(lines)
    d.rectangle([0, y - 14, W, H], fill=(8, 10, 14))
    for i, line in enumerate(lines):
        d.text((60, y + 32 * i), line, font=F_TXT, fill=FG)


def load(rundir):
    serial = qv.read_text(os.path.join(rundir, "serial.log"))
    host = qv.read_text(os.path.join(rundir, "host_bridge.txt"))
    data = qv.parse(serial)
    lines = serial.replace("\r", "").split("\n")
    # the demo: frames dumped after "QV RUN demo.dump", error frames after "error-only node"
    frames, errors, in_dump, in_err = [], [], False, False
    for l in lines:
        if "QV RUN demo.dump" in l:
            in_dump = True
        elif "QV EXIT demo.dump" in l:
            in_dump = False
        elif "error-only node" in l:
            in_err = True
        elif l.startswith("QV DEMO error frames in total"):
            in_err = False
        m = DUMP_FRAME.search(l)
        if in_dump and m:
            frames.append((float(m.group(1)), m.group(2), int(m.group(5)), int(m.group(6))))
        m = DUMP_ERR.search(l)
        if in_err and m:
            errors.append((float(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4)),
                           m.group(5)))
    uname = next((l.strip() for l in lines if l.startswith("QNX ") and "8.0" in l), "")
    gens = [l.strip()[3:] for l in lines if l.startswith("QV GEN")]
    stats = [l.strip()[3:] for l in lines if l.startswith("QV STAT")]
    totals = next((l.strip()[3:] for l in lines if l.startswith("QV DEMO error frames in total")), "")
    status = qv.read_text(os.path.join(rundir, "qemu_status.txt")).strip()
    return data, frames, errors, uname, gens, stats, totals, host, status


def detail(data, name):
    for p in data["pass"]:
        n, _, rest = p.partition(" ")
        if n == name:
            return rest
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("rundir")
    ap.add_argument("out")
    ap.add_argument("--repo", default="github.com/kbns2000/qvcan")
    args = ap.parse_args()
    data, frames, errors, uname, gens, stats, totals, host, status = load(args.rundir)
    npass, nfail = len(data["pass"]), len(data["fail"])
    scenes = []

    # 1 title
    im, d = frame()
    d.text((60, 180), "qvcan", font=F_BIG, fill=ACC)
    d.text((60, 262), "A virtual CAN bus for QNX 8.0", font=F_H1, fill=FG)
    d.text((60, 320), "A resource manager: /dev/vcan0, /dev/vcan1 ... every open() is a node.",
           font=F_TXT, fill=FG)
    d.text((60, 356), "Arbitration, bit timing with stuffing and CRC, error counters and bus-off,",
           font=F_TXT, fill=FG)
    d.text((60, 390), "so CAN software can be developed and tested on QNX without hardware.",
           font=F_TXT, fill=FG)
    d.text((60, 460), "Everything on screen is parsed from the console log of a QNX 8.0 guest in QEMU.",
           font=F_SMALL, fill=DIM)
    d.text((60, 486), uname, font=F_MONO_S, fill=DIM)
    scenes.append((im, 7))

    # 2 how it works
    im, d = frame()
    header(d, "How it works", "src/devvcan.c (QNX resource manager) + src/vcan_core.c (bus model)")
    boxes = [("vcan gen 0C0", 150), ("vcan gen 2B0 -F 3", 250), ("your program", 350),
             ("vcanbridge (UDP)", 450)]
    for label, y in boxes:
        d.rounded_rectangle([70, y, 360, y + 64], 10, fill=PANEL, outline=ACC)
        d.text((90, y + 20), label, font=F_H2, fill=FG)
        d.line([360, y + 32, 470, 330], fill=DIM, width=2)
    d.rounded_rectangle([470, 230, 850, 430], 12, fill=PANEL, outline=GOOD, width=2)
    d.text((492, 246), "devvcan", font=F_H1, fill=GOOD)
    for i, s in enumerate(["/dev/vcan0  /dev/vcan1", "one dispatch thread", "timer pulse at frame end",
                           "deferred MsgReply"]):
        d.text((492, 296 + 30 * i), s, font=F_SMALL, fill=FG)
    for i, s in enumerate(["open()  = a node", "write() = queue frames", "read()  = frames of others",
                           "select() / ionotify()", "devctl(): filters, rate,", "  faults, stats, reset",
                           "signal / TimerTimeout", "  unblocks: EINTR"]):
        d.text((890, 200 + 34 * i), s, font=F_MONO, fill=FG)
    caption(d, "Frames are 32-byte structs. The bus is a time line: a frame occupies it for its exact "
               "number of bits at the bit rate, and a QNX timer pulse ends it.")
    scenes.append((im, 11))

    # 3 test results
    im, d = frame()
    header(d, f"Tests on QNX 8.0: {npass} PASS, {nfail} FAIL",
           "tests/vcan_test.c, run at boot by vm/run.sh")
    rows = [("PASS", p) for p in data["pass"]] + [("FAIL", f) for f in data["fail"]]
    y = 110
    for res, text in rows[:19]:
        name, _, rest = text.partition(" ")
        d.rounded_rectangle([60, y, 128, y + 24], 5, fill=GOOD if res == "PASS" else BAD)
        d.text((70, y + 3), res, font=F_MONO_S, fill=FG)
        d.text((140, y + 2), name, font=F_MONO, fill=ACC)
        while rest and d.textlength(rest, font=F_MONO_S) > W - 60 - 370:
            rest = rest[:-2].rstrip() + "\u2026"
        d.text((370, y + 3), rest, font=F_MONO_S, fill=FG)
        y += 25
    caption(d, "Arbitration order, bit timing, back pressure, overflow, error passive and bus-off, "
               "select(), signals and kernel timeouts, 500 open/close cycles, the UDP bridge.")
    scenes.append((im, 14))

    # 4 arbitration and timing details
    im, d = frame()
    header(d, "On the wire: who wins, and how long a frame takes")
    y = 120
    for name, label in [("arbitration.order", "Arbitration"), ("arbitration.fifo", "One node's queue"),
                        ("timing.stuffing", "Bit stuffing"), ("timing.bitrate", "Bit rate"),
                        ("write.backpressure", "Back pressure")]:
        t = detail(data, name)
        if not t:
            continue
        d.text((60, y), label, font=F_H2, fill=ACC)
        for i, part in enumerate(wrap(d, t, F_TXT, W - 380)):
            d.text((320, y + 2 + 30 * i), part, font=F_TXT, fill=FG)
        y += 30 * len(wrap(d, t, F_TXT, W - 380)) + 22
    caption(d, "The lowest identifier wins as on a real bus; frame lengths include stuff bits computed "
               "over the real CRC-15, so the same payload can take different time.")
    scenes.append((im, 14))

    # 5 bus time line of the demo
    im, d = frame()
    header(d, "The demo bus at 125 kbit/s: four nodes, 0x2B0 is faulty",
           "frames from 'vcan dump'; red: frames destroyed by the faulty node (error-only node)")
    if frames:
        t0 = frames[0][0] - frames[0][2] / 125000.0
        span = 0.060
        ids = []
        for _, fid, _, _ in frames:
            if fid not in ids:
                ids.append(fid)
        x0, x1 = 230, W - 60
        lane = {fid: 130 + 52 * i for i, fid in enumerate(ids)}
        bus_y = 130 + 52 * len(ids) + 14
        for fid, y in lane.items():
            d.text((60, y + 8), "0x" + fid, font=F_MONO, fill=FG)
            d.line([x0, y + 40, x1, y + 40], fill=(40, 46, 58))
        d.text((60, bus_y + 8), "bus", font=F_MONO, fill=FG)
        for i, (end, fid, bits, _) in enumerate(frames):
            start = end - bits / 125000.0
            if start - t0 > span:
                break
            xa = x0 + (start - t0) / span * (x1 - x0)
            xb = x0 + (end - t0) / span * (x1 - x0)
            col = ID_COLOURS[ids.index(fid) % len(ID_COLOURS)]
            d.rectangle([xa, lane[fid] + 6, max(xb, xa + 2), lane[fid] + 36], fill=col)
            d.rectangle([xa, bus_y + 6, max(xb, xa + 2), bus_y + 36], fill=col)
        for ts, kind, node, tec, state in errors:
            if t0 <= ts <= t0 + span and "2B0" in lane:
                x = x0 + (ts - t0) / span * (x1 - x0)
                d.rectangle([x - 3, lane["2B0"] + 2, x + 3, lane["2B0"] + 40], fill=BAD)
                d.rectangle([x - 3, bus_y + 2, x + 3, bus_y + 40], fill=BAD)
        for k in range(7):
            x = x0 + k / 6 * (x1 - x0)
            d.text((x - 16, bus_y + 50), f"{k * 10} ms", font=F_SMALL, fill=DIM)
        for i, g in enumerate(gens):
            d.text((60 + 400 * (i // 3), bus_y + 84 + 20 * (i % 3)), g, font=F_MONO_S, fill=DIM)
    caption(d, "Each block is one frame on the wire, as long as its bits take (124 bits of an 8-byte "
               "frame = 1 ms). Nodes queue independently; the bus carries one frame at a time.")
    scenes.append((im, 15))

    # 6 error counter of the faulty node
    im, d = frame()
    header(d, "Error confinement: the faulty node's transmit error counter",
           "every error frame seen by the error-only node (vcan dump -e); +8 per error, -1 per good frame")
    gx0, gx1, gy0, gy1 = 120, W - 80, 126, 500
    d.rectangle([gx0, gy0, gx1, gy1], outline=(50, 56, 68))
    pts = [(ts, tec, kind) for ts, kind, node, tec, state in errors]
    if pts:
        ta, tb = pts[0][0], pts[-1][0]
        top = 272

        def gy(v):
            return gy1 - v / top * (gy1 - gy0)
        for lvl, lab, col in [(128, "error passive (128)", WARN), (256, "bus-off (256)", BAD)]:
            d.line([gx0, gy(lvl), gx1, gy(lvl)], fill=col, width=1)
            d.text((gx0 + 8, gy(lvl) - 22), lab, font=F_SMALL, fill=col)
        prev = None
        for ts, tec, kind in pts:
            x = gx0 + (ts - ta) / max(tb - ta, 1e-9) * (gx1 - gx0)
            y = gy(min(tec, top))
            if prev and tec >= prev[2]:
                d.line([prev[0], prev[1], x, y], fill=ACC, width=2)
            col = BAD if "bus-off" in kind else ACC
            d.ellipse([x - 4, y - 4, x + 4, y + 4], fill=col)
            prev = (x, y, tec)
        d.text((gx0, gy1 + 10), f"{ta:.2f} s", font=F_SMALL, fill=DIM)
        d.text((gx1 - 70, gy1 + 10), f"{tb:.2f} s", font=F_SMALL, fill=DIM)
        d.text((gx0 + 300, gy1 + 10), totals, font=F_SMALL, fill=FG)
    caption(d, "The node destroys every third frame it sends: its counter climbs, it becomes error "
               "passive at 128 and bus-off at 256; its queue is dropped and write() fails with EIO "
               "until the generator resets it. Other nodes keep sending.")
    scenes.append((im, 16))

    # 7 latency
    im, d = frame()
    header(d, "Latency through the resource manager (QEMU software CPU)",
           "measured with ClockCycles() in the guest; bars: p50 (green), p90, p99 (red)")
    lats = [LAT.search(x) for x in data["lat"]]
    lats = [m for m in lats if m]
    if lats:
        mx = max(float(m.group(5)) for m in lats)
        y = 150
        for m in lats:
            d.text((60, y), m.group(1), font=F_MONO, fill=FG)
            for j, col in [(3, GOOD), (4, WARN), (5, BAD)]:
                v = float(m.group(j))
                d.rectangle([520, y + 26 + (j - 3) * 12, 520 + v / mx * 640, y + 34 + (j - 3) * 12],
                            fill=col)
            d.text((60, y + 24), f"p50 {m.group(3)} us   p99 {m.group(5)} us   n={m.group(2)}",
                   font=F_MONO_S, fill=DIM)
            y += 84
    tp = next((x for x in data["lat"] if x.startswith("throughput")), "")
    d.text((60, y + 6), tp, font=F_MONO, fill=FG)
    caption(d, "QEMU emulates the CPU in software, so these are upper bounds, not hardware figures. "
               "The bridge round trip includes QEMU's user network and the host program.")
    scenes.append((im, 12))

    # 8 bridge
    im, d = frame()
    header(d, "vcanbridge: the QEMU host is one more node",
           "guest: vcan_test bridge -> /dev/vcan0 -> vcanbridge -> UDP -> scripts/qv_host.py (host)")
    hl = [l for l in host.split("\n") if l.strip()]
    rx = [l for l in hl if " rx " in l or " tx " in l][:10]
    y = 130
    d.text((60, y), "host_bridge.txt (host side):", font=F_H2, fill=ACC)
    y += 36
    for l in rx + [hl[-1] if hl else ""]:
        d.text((80, y), l.strip()[:100], font=F_MONO, fill=FG)
        y += 24
    bp = detail(data, "bridge.udp")
    if bp:
        d.text((60, y + 20), "guest: " + bp, font=F_TXT, fill=GOOD)
    caption(d, "The host answers each 0x3xx frame with id + 0x100 and the data reversed; the guest "
               "test checks every answer.")
    scenes.append((im, 11))

    # 9 end
    im, d = frame()
    d.text((60, 190), "qvcan", font=F_BIG, fill=ACC)
    d.text((60, 270), args.repo, font=F_H1, fill=FG)
    for i, s in enumerate(["QNX SDP 8.0, qcc for x86_64 and aarch64le; the run: QEMU x86_64 (" +
                           status + ").",
                           "Apache-2.0. Written for the QNX hackathon, November 2026.",
                           "Results from QEMU emulation: behaviour, not hardware performance."]):
        d.text((60, 350 + 38 * i), s, font=F_TXT, fill=DIM if i == 2 else FG)
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
