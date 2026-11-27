#!/usr/bin/env python3
"""Add a spoken narration to a silent demo video (Windows, built-in voices).

    python tools/narrate.py VIDEO.mp4 NARRATION.json OUT.mp4 [--voice "Microsoft Zira Desktop"]

NARRATION.json is written by tools/make_video.py next to the video: a list of
{"start": s, "end": s, "text": "..."}; the on-screen caption of each scene is
the same text, so the captions are the subtitles of the narration.

Each line is spoken by Windows' own speech synthesizer (System.Speech, no
download), placed at its start time, and the track is muxed into the video
with ffmpeg. A line that does not fit before its end time stops the script
with a list of the lines that are too long.
Standard library only. SPDX-License-Identifier: Apache-2.0
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import wave


def speak(text, path, voice, rate):
    ps = ("Add-Type -AssemblyName System.Speech;"
          "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer;"
          f"$s.SelectVoice('{voice}'); $s.Rate = {rate};"
          f"$s.SetOutputToWaveFile('{path}');"
          "$s.Speak([Console]::In.ReadToEnd()); $s.Dispose()")
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], input=text.encode("utf-8"),
                   check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("video")
    ap.add_argument("narration")
    ap.add_argument("out")
    ap.add_argument("--voice", default="Microsoft Zira Desktop")
    ap.add_argument("--rate", type=int, default=0)
    a = ap.parse_args()
    lines = json.load(open(a.narration, encoding="utf-8"))
    ffmpeg = shutil.which("ffmpeg") or r"C:\ffmpeg\bin\ffmpeg.exe"
    tmp = tempfile.mkdtemp()
    clips, too_long, fmt = [], [], None
    for i, ln in enumerate(lines):
        p = os.path.join(tmp, f"l{i:02d}.wav")
        speak(ln["text"], p, a.voice, a.rate)
        with wave.open(p) as w:
            params = w.getparams()
            frames = w.readframes(w.getnframes())
        fmt = fmt or params
        dur = params.nframes / params.framerate
        slot = ln["end"] - ln["start"]
        print(f"{i:2d} {dur:5.2f} s of {slot:5.2f} s  {ln['text'][:70]}")
        if dur > slot - 0.2:
            too_long.append((i, dur, slot))
        clips.append((ln["start"], frames))
    if too_long:
        sys.exit("narrate.py: lines too long for their scene: " +
                 ", ".join(f"#{i} {d:.2f}s > {s - 0.2:.2f}s" for i, d, s in too_long))
    # one track: silence, then each clip at its start time (same format for all clips)
    bpf = fmt.sampwidth * fmt.nchannels
    total = int((lines[-1]["end"] + 1) * fmt.framerate)
    track = bytearray(total * bpf)
    for start, frames in clips:
        o = int(start * fmt.framerate) * bpf
        track[o:o + len(frames)] = frames
    wav = os.path.join(tmp, "narration.wav")
    with wave.open(wav, "wb") as w:
        w.setparams(fmt)
        w.writeframes(bytes(track))
    subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", a.video, "-i", wav, "-map", "0:v",
                    "-map", "1:a", "-c:v", "copy", "-c:a", "aac", "-b:a", "128k", "-shortest",
                    a.out], check=True)
    print(f"{a.out}: {len(lines)} spoken lines")


if __name__ == "__main__":
    main()
