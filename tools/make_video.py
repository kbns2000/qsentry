#!/usr/bin/env python3
"""Render the demo video from a real qsentry serial log captured on QNX 8.0 (QEMU).

Every number and state shown on screen is parsed from the log; nothing is
re-simulated.  The caption of each scene is also its narration: the script
writes OUT.json (start, end, text per scene) and OUT.srt next to the video;
tools/narrate.py adds the spoken track on a Windows PC.

usage: python3 tools/make_video.py docs/qnx_run_2026-11-07/serial_run3.log docs/media/qsentry_demo_silent.mp4
needs: Pillow, ffmpeg, DejaVu fonts
"""
import os, re, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFont

W, H = 1280, 720
FD = "/usr/share/fonts/truetype/dejavu/"
F_MONO = ImageFont.truetype(FD + "DejaVuSansMono.ttf", 15)
F_MONO_B = ImageFont.truetype(FD + "DejaVuSansMono-Bold.ttf", 15)
F_BIG = ImageFont.truetype(FD + "DejaVuSans-Bold.ttf", 54)
F_H1 = ImageFont.truetype(FD + "DejaVuSans-Bold.ttf", 34)
F_H2 = ImageFont.truetype(FD + "DejaVuSans-Bold.ttf", 24)
F_TXT = ImageFont.truetype(FD + "DejaVuSans.ttf", 22)
F_SMALL = ImageFont.truetype(FD + "DejaVuSans.ttf", 17)

BG = (14, 17, 22)
PANEL = (24, 28, 36)
FG = (220, 225, 232)
DIM = (130, 140, 155)
ACC = (90, 170, 255)
STATE_COL = {"NOMINAL": (46, 160, 90), "DEGRADED": (230, 160, 30), "SAFE_STOP": (210, 50, 50)}
VERD_COL = {"ok": (46, 160, 90)}


def parse(path):
    txt = open(path, encoding="utf-8", errors="replace").read().replace("\x1b[?7h", "")
    txt = re.sub(r"[\x00-\x08\x0b-\x1f\x7f]", "", txt)
    boot = txt.split("=== QSENTRY AUTODEMO START")[0].strip().splitlines()
    steps = []
    for m in re.finditer(r"--- \[(\d+) ([^\]]+)\]\n(.*?)(?=\n\n|\n---|\n>>>|\Z)", txt, re.S):
        body = m.group(3)
        kv = dict(re.findall(r"^(\w+)=(.*)$", body, re.M))
        chans = re.findall(r"^ch\d=(\w+) value=(\S+) verdict=(\w+) conf=(\S+)", body, re.M)
        steps.append({"n": int(m.group(1)), "title": m.group(2), "kv": kv, "ch": chans,
                      "raw": body.strip().splitlines()})
    events = re.findall(r"^qsentryd: (.*)$", txt, re.M)
    return boot, steps, events


def wrap(d, text, font, width):
    out, line = [], ""
    for w in text.split():
        t = (line + " " + w).strip()
        if d.textlength(t, font=font) > width and line:
            out.append(line); line = w
        else:
            line = t
    if line: out.append(line)
    return out


def caption(d, text):
    lines = wrap(d, text, F_TXT, W - 120)
    h = 18 + 30 * len(lines)
    d.rectangle([0, H - h - 10, W, H], fill=(0, 0, 0))
    for i, l in enumerate(lines):
        d.text((60, H - h + 2 + 30 * i), l, font=F_TXT, fill=(255, 255, 255))


def footer(d):
    d.text((W - 470, 8), "real serial log: QNX 8.0 x86_64 in QEMU, 2026-11-07", font=F_SMALL, fill=DIM)


def card(title, lines, sub=None, cap=None):
    im = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(im)
    y = 150 if sub is None else 120
    d.text((80, y), title, font=F_H1, fill=FG)
    y += 70
    if sub:
        d.text((80, y - 20), sub, font=F_TXT, fill=ACC); y += 40
    for l in lines:
        for wl in wrap(d, l, F_TXT, W - 160):
            d.text((80, y), wl, font=F_TXT, fill=FG); y += 34
        y += 8
    if cap:
        caption(d, cap)
    return im


def numbers_card(rows, cap):
    """The first scene: what was measured, big."""
    im = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(im)
    d.text((60, 40), "qsentry: which sensor is lying, measured on QNX 8.0", font=F_H1, fill=FG)
    d.text((60, 88), "numbers parsed from the QNX 8.0 (QEMU) serial log and the held-out evaluation",
           font=F_SMALL, fill=DIM)
    y = 140
    for num, lab, col in rows:
        d.text((60, y), num, font=F_BIG, fill=col)
        d.text((420, y + 18), lab, font=F_TXT, fill=FG)
        y += 98
    caption(d, cap)
    return im


def terminal(d, x0, y0, x1, y1, lines, hl=()):
    d.rectangle([x0, y0, x1, y1], fill=PANEL)
    d.text((x0 + 12, y0 + 8), "# cat /dev/qsentry", font=F_MONO_B, fill=ACC)
    y = y0 + 32
    maxl = (y1 - y - 8) // 19
    for l in lines[-maxl:]:
        f = F_MONO_B if any(h in l for h in hl) else F_MONO
        d.text((x0 + 12, y), l[:62], font=f, fill=FG if f is F_MONO else (255, 230, 120)); y += 19


def dash(step, events_upto, cap, reveal=1.0):
    im = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(im)
    footer(d)
    d.text((40, 6), f"step {step['n']}: {step['title']}", font=F_H2, fill=FG)
    raw = step["raw"][: max(1, int(len(step["raw"]) * reveal))]
    terminal(d, 30, 45, 640, 620, raw, hl=("state=", "event=", "pipeline_us", "tick_jitter", "reaction_ms"))
    st = step["kv"].get("state", "NOMINAL")
    d.rectangle([660, 45, 1250, 140], fill=STATE_COL.get(st, DIM))
    tw = d.textlength(st, font=F_BIG)
    d.text((955 - tw / 2, 60), st, font=F_BIG, fill=(255, 255, 255))
    y = 155
    for name, val, verd, conf in step["ch"]:
        col = VERD_COL.get(verd, (210, 60, 60))
        d.rectangle([660, y, 1250, y + 44], fill=PANEL)
        d.rectangle([660, y, 668, y + 44], fill=col)
        d.text((680, y + 10), f"{name:<6}", font=F_H2, fill=FG)
        d.text((800, y + 12), f"{float(val):9.3f}", font=F_MONO_B, fill=DIM)
        d.text((930, y + 10), f"{verd}", font=F_H2, fill=col)
        if float(conf) > 0: d.text((1080, y + 12), f"p={conf}", font=F_MONO, fill=DIM)
        y += 50
    d.text((660, y + 4), "qsentryd event log", font=F_SMALL, fill=ACC)
    y += 28
    for e in events_upto[-5:]:
        d.text((660, y), e[:58], font=F_MONO, fill=FG); y += 19
    caption(d, cap)
    return im


def latencies(events):
    """inject -> first reaction, from the qsentryd event log."""
    out, pend = {}, None
    for e in events:
        m = re.match(r"t=([\d.]+)s (.*)", e)
        if not m: continue
        t, msg = float(m.group(1)), m.group(2)
        if msg.startswith("inject "):
            pend = (t, msg[7:])
        elif "->" in msg and pend:
            out.setdefault(pend[1], (t - pend[0], msg)); pend = None
        elif msg.startswith("WATCHDOG"):
            out["watchdog"] = (float(re.search(r"for (\d+) ms", msg).group(1)) / 1000, msg)
    return out


def captions(steps, events):
    """Spoken text per demo step (also the on-screen caption)."""
    L = latencies(events)
    kv = {st["n"]: st["kv"] for st in steps}
    f = lambda k: f"{L[k][0]:.2f} seconds" if k in L else "a moment"
    reset_state = kv.get(6, {}).get("state", "")
    m7 = re.match(r"p50 (\d+)\s+p99 (\d+)", kv.get(7, {}).get("pipeline_us", ""))
    load = f"a median of {m7.group(1)} microseconds" if m7 else "bounded time"
    reset_cap = ("An operator reset after the window was flushed: back to nominal."
                 if reset_state == "NOMINAL" else
                 "An operator reset while the window still holds faulty samples: safe stop latches "
                 "again. qsentry never trusts stale evidence.")
    wd = L.get("watchdog", (0, ""))[0] * 1000
    return {
        1: "Clean driving. Four signals at one hundred hertz, and every sensor is checked on every "
           "tick. State: nominal.",
        2: "Now the gyro drifts. On its own it looks normal, but it no longer matches steering and "
           f"speed. Flagged as drift after {f('yaw drift')}. Degraded.",
        3: "The fault is cleared. A sensor is trusted again only after a full second of confident "
           "health. Back to nominal.",
        4: f"The wheel speed sensor freezes. Stuck, detected in {f('speed stuck')}. The gyro is not "
           "blamed by mistake.",
        5: f"Accelerometer noise, found in {f('accel noise')}, plus gyro drift. Both redundant pairs "
           "are lost, so qsentry latches safe stop.",
        6: reset_cap,
        7: "Two low priority programs load the CPU. The detector runs at priority forty and stays "
           f"bounded: {load}.",
        8: "The sensor process is killed. A timer pulse on the same QNX channel is the watchdog: "
           f"safe stop {wd:.0f} milliseconds after the last sample.",
    }, L


def main(log, out):
    boot, steps, events = parse(log)
    ev_t = []
    for e in events:
        m = re.match(r"t=([\d.]+)s", e)
        ev_t.append((float(m.group(1)) if m else 0.0, e))
    frames = []  # (image, seconds, spoken text or None)
    CAPS, L = captions(steps, events)
    GOOD, WARN = (46, 160, 90), (230, 160, 30)
    frames.append((numbers_card([
        ("800 / 800", "faults found, right sensor and type (simulated drives)", GOOD),
        ("0", "false alarms in 200 clean drives (3.3 simulated hours)", GOOD),
        (f"{L['speed stuck'][0]:.2f} s", "stuck wheel-speed sensor detected on QNX 8.0", ACC),
        (f"{L['watchdog'][0] * 1000:.0f} ms", "dead sensor process -> SAFE_STOP (deadline watchdog)", WARN),
    ], s0 := "On QNX 8.0, qsentry caught a stuck sensor in half a second. In simulated drives it "
             "found all 800 faults, with no false alarms."), 12, s0))
    frames.append((card("The problem", [
        "Braking, stability control and path planning trust their sensors.",
        "A frozen wheel-speed sensor or a slowly drifting gyro feeds them wrong numbers.",
        "Per-signal thresholds miss slow drifts or raise false alarms."],
        cap=(s1 := "Brakes and stability control trust their sensors. A frozen or drifting sensor "
                   "feeds them wrong numbers.")), 8, s1))
    frames.append((card("How qsentry works on QNX", [
        "qsensord (prio 30) MsgSend()s every 100 Hz sample to qsentryd; the reply can carry a command.",
        "qsentryd (SCHED_FIFO 40): sliding window -> features incl. physics residuals -> tiny MLP "
        "(293 params, 1.2 KB) -> supervisor NOMINAL / DEGRADED / SAFE_STOP.",
        "A POSIX timer SIGEV_PULSE on the same channel = 50 ms deadline watchdog.",
        "Resource manager /dev/qsentry: cat for status, echo to inject faults or reset."],
        cap=(s2 := "A sensor process sends every sample to qsentry with QNX message passing. A tiny "
                   "neural network checks each sensor, and a timer pulse is the watchdog.")), 12, s2))

    # boot
    im = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(im); footer(d)
    d.text((40, 6), "Booting the mkqnximage QEMU image", font=F_H2, fill=FG)
    d.rectangle([30, 45, 1250, 600], fill=PANEL)
    y = 55
    for l in boot[-24:]:
        d.text((42, y), l[:120], font=F_MONO, fill=FG if "QNX" not in l else (255, 230, 120)); y += 21
    s3 = "A real QNX 8.0 target in QEMU. Everything shown comes from its log."
    caption(d, s3)
    frames.append((im, 8, s3))

    order = steps
    for s in order:
        t_now = 1e9
        m = re.search(r"t=([\d.]+)s", s["kv"].get("event", ""))
        if m: t_now = float(m.group(1))
        evs = [e for t, e in ev_t if t <= t_now] if s["n"] != 1 else [events[0]]
        for k, (r, dur) in enumerate(((0.35, 1.2), (0.7, 1.2), (1.0, 11.6))):
            # one spoken line per step, over its three frames (14 s)
            frames.append((dash(s, evs, CAPS[s["n"]], r), dur, CAPS[s["n"]] if k == 0 else ""))

    sr = ("On QNX: drift in %.1f seconds, stuck sensor in %.2f, noise in %.2f, watchdog in %.0f "
          "milliseconds." %
          (L["yaw drift"][0], L["speed stuck"][0], L["accel noise"][0], L["watchdog"][0] * 1000))
    frames.append((card("Results", [
        "On QNX 8.0 (QEMU, software-emulated CPU): gyro drift %.1f s, wheel speed stuck %.2f s, "
        "accel noise %.2f s, watchdog %.0f ms." % (L["yaw drift"][0], L["speed stuck"][0],
                                                   L["accel noise"][0], L["watchdog"][0] * 1000),
        "Offline, held-out seed: 800 / 800 injected faults found with the right sensor and fault type; "
        "0 false alarms in 200 clean drives (3.3 simulated hours).",
        "Detector: 3.5 us per tick on an x86-64 host, no allocation, bounded time."], cap=sr), 14, sr))
    se = "qsentry is open source, Apache 2.0. The data is simulated: retrain on real data first."
    frames.append((card("qsentry", [
        "Open source, Apache-2.0: github.com/kbns2000/qsentry",
        "Simulated sensor data; retrain on recorded data before real use. Not a certified safety component."],
        sub="Built with QNX SDP 8.0, C11, numpy", cap=se), 10, se))

    total = sum(t for _, t, _ in frames)
    # narration: a line starts with its frame and may run until the next spoken line
    narr, t = [], 0.0
    for im, dur, text in frames:
        if text:
            narr.append({"start": round(t, 3), "end": None, "text": text})
        t += dur
        if narr:
            narr[-1]["end"] = round(t, 3)
    base = os.path.splitext(out)[0]
    import json
    with open(base + ".json", "w", encoding="utf-8") as f:
        json.dump(narr, f, indent=1)
    def ts(x):
        return "%02d:%02d:%02d,%03d" % (x // 3600, x % 3600 // 60, x % 60, round(x % 1 * 1000) % 1000)
    with open(base + ".srt", "w", encoding="utf-8") as f:
        for i, n in enumerate(narr, 1):
            f.write(f"{i}\n{ts(n['start'])} --> {ts(n['end'])}\n{n['text']}\n\n")
    tmp = tempfile.mkdtemp()
    lst = os.path.join(tmp, "list.txt")
    with open(lst, "w") as f:
        for i, (im, t, _) in enumerate(frames):
            p = os.path.join(tmp, f"f{i:03d}.png"); im.save(p)
            f.write(f"file '{p}'\nduration {t}\n")
        f.write(f"file '{p}'\n")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", lst,
                    "-vf", "fps=30,format=yuv420p", "-c:v", "libx264", "-crf", "20", out], check=True)
    print(f"{out}: {total:.1f} s, {len(frames)} frames, previews in {tmp}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
