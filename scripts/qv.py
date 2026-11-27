#!/usr/bin/env python3
"""Build qvcan for QNX 8.0, put it into a QEMU image, run the tests, report.

    python scripts/qv.py build  [--arch x86_64,aarch64le]
    python scripts/qv.py image  [--vmroot DIR]
    python scripts/qv.py run    [--vmroot DIR] [--timeout MIN]
    python scripts/qv.py report [RUN_DIR]
    python scripts/qv.py all
    python scripts/qv.py watch  --branch BRANCH [--until YYYY-MM-DD]

Run the QNX SDP 8.0 environment script first (qnxsdp-env.bat, or
"source qnxsdp-env.sh"), so that QNX_HOST and QNX_TARGET are set.

build   qcc for each CPU: out/<arch>/{devvcan,vcan,vcanbridge,vcan_test};
        compiler output in results/build_<arch>.log
image   a QEMU x86_64 image with mkqnximage in <vmroot>: the programs in
        /data/qv and vm/run.sh, which runs at boot
run     boots it in QEMU (software emulation) with UDP port forwarding for
        the bridge test, runs scripts/qv_host.py as the host end of the
        bridge, waits until the guest shuts down; the console goes to
        results/runs/<time>/serial.log
report  results/runs/<time>/SUMMARY.md from the "QV ..." lines
watch   for unattended runs: whenever BRANCH on origin has a new commit (not
        one of its own result commits), fast-forwards this clone to it, runs
        "all", commits results/ and pushes. Stops at --until.

Standard library only.
SPDX-License-Identifier: Apache-2.0
"""
import argparse
import datetime
import glob
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WINDOWS = os.name == "nt"
ARCHES = ("x86_64", "aarch64le")
PROGRAMS = {
    "devvcan": ["src/devvcan.c", "src/vcan_core.c"],
    "vcan": ["tools/vcan.c"],
    "vcanbridge": ["tools/vcanbridge.c"],
    "vcan_test": ["tests/vcan_test.c"],
}
LIBS = {"vcanbridge": ["-lsocket"]}
CFLAGS = ["-O2", "-std=gnu11", "-Wall", "-Wextra", "-Iinclude", "-Isrc"]
BRIDGE_GUEST_PORT = 47001  # vcanbridge in the guest; QEMU forwards host 127.0.0.1:47001 to it
BRIDGE_HOST_PORT = 47002   # scripts/qv_host.py; the guest reaches it as 10.0.2.2:47002
DEFAULT_VMROOT = os.path.join(ROOT, "out", "vm")
HOSTNAME = socket.gethostname()
HOME = os.path.expanduser("~")


def die(msg):
    sys.exit("qv.py: " + msg)


def say(msg):
    print("qv.py: " + msg, flush=True)


def scrub(text):
    """No user or machine names in results/: the home directory as ~, the
    host name as <host>."""
    for home in {HOME, HOME.replace("\\", "/")}:
        if len(home) > 3:
            text = re.sub(re.escape(home), "~", text, flags=re.I)
    if len(HOSTNAME) > 2:
        text = re.sub(r"\b" + re.escape(HOSTNAME) + r"\b", "<host>", text, flags=re.I)
    return text


def read_text(path):
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace").replace("\r", "")
    except OSError:
        return ""


def write_text(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(scrub(text))


def qnx_env():
    host, target = os.environ.get("QNX_HOST"), os.environ.get("QNX_TARGET")
    if not host or not target:
        die("QNX_HOST and QNX_TARGET are not set: run the SDP environment script first")
    return host, target


def tool(name):
    path = shutil.which(name)
    if not path:
        die(f"{name} not found in PATH")
    if WINDOWS and path.lower().endswith((".bat", ".cmd")):
        return [os.environ.get("COMSPEC", r"C:\Windows\System32\cmd.exe"), "/c", path]
    return [path]


def run(cmd, cwd=None, log=None, check=True):
    print("$ " + " ".join(cmd), flush=True)
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = p.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    sys.stdout.write(text)
    if log:
        with open(log, "a", encoding="utf-8", newline="\n") as fh:
            fh.write(scrub("$ " + " ".join(cmd) + "\n" + text))
    if check and p.returncode != 0:
        die(f"failed ({p.returncode}): {' '.join(cmd)}")
    return p.returncode, text


def git(*args, check=True):
    p = subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True)
    if check and p.returncode != 0:
        die("git " + " ".join(args) + ": " + p.stderr.strip())
    return p.stdout.strip()


def source_commit():
    try:
        head = git("rev-parse", "--short=12", "HEAD")
        changed = git("status", "--porcelain", "--untracked-files=no", "--", ".",
                      ":(exclude)results")
    except SystemExit:
        return "unknown"
    return head + ("+changes" if changed else "")


# ------------------------------------------------------------------ build
def cmd_build(args):
    qnx_env()
    os.makedirs(os.path.join(ROOT, "results"), exist_ok=True)
    commit = source_commit()
    failed = []
    for arch in args.arch:
        log = os.path.join(ROOT, "results", f"build_{arch}.log")
        if os.path.exists(log):
            os.remove(log)
        with open(log, "w", newline="\n") as fh:
            fh.write(f"qvcan build for {arch}, sources {commit}\n")
        run(tool("qcc") + ["-V"], log=log, check=False)  # the compiler versions
        dest = os.path.join(ROOT, "out", arch)
        os.makedirs(dest, exist_ok=True)
        for prog, srcs in PROGRAMS.items():
            out = os.path.join(dest, prog)
            if os.path.exists(out):
                os.remove(out)
            rc, _ = run(tool("qcc") + [f"-Vgcc_nto{arch}"] + CFLAGS +
                        [f'-DQV_COMMIT="{commit}"', "-o", out] + srcs + LIBS.get(prog, []),
                        cwd=ROOT, log=log, check=False)
            if rc != 0:
                failed.append(f"{arch}/{prog}")
        warnings = sum(1 for line in read_text(log).split("\n") if "warning:" in line)
        say(f"{arch}: {len(PROGRAMS)} programs in {dest}, {warnings} compiler warnings")
    if failed:
        die("build failed: " + ", ".join(failed) + " (see results/build_<arch>.log)")


# ------------------------------------------------------------------ image
def cmd_image(args):
    qnx_env()
    arch = "x86_64"
    vm = args.vmroot
    snippets = os.path.join(vm, "local", "snippets")
    payload = os.path.join(vm, "payload")
    shutil.rmtree(payload, ignore_errors=True)
    os.makedirs(snippets, exist_ok=True)
    os.makedirs(payload)
    files = []
    for prog in PROGRAMS:
        src = os.path.join(ROOT, "out", arch, prog)
        if not os.path.exists(src):
            die(f"{src} is missing: run 'qv.py build' first")
        files.append((prog, src, "0755"))
    with open(os.path.join(ROOT, "vm", "run.sh"), "rb") as fh:
        script = fh.read().replace(b"\r\n", b"\n")
    with open(os.path.join(payload, "run.sh"), "wb") as fh:
        fh.write(script)
    files.append(("run.sh", os.path.join(payload, "run.sh"), "0755"))
    with open(os.path.join(snippets, "data_files.custom"), "w", newline="\n") as fh:
        for dest, src, perms in files:
            fh.write(f"[uid=0 gid=0 perms={perms}] qv/{dest}=" +
                     os.path.abspath(src).replace("\\", "/") + "\n")
    shutil.copyfile(os.path.join(ROOT, "vm", "post_start.custom"),
                    os.path.join(snippets, "post_start.custom"))
    run(tool("mkqnximage") + ["--noprompt", "--force", "--type=qemu", "--arch=" + arch,
                              "--hostname=qvcan", "--ssh-ident=none", "--build"], cwd=vm)
    say(f"image in {os.path.join(vm, 'output')}")


# -------------------------------------------------------------------- run
def qemu_binary(args):
    exe = "qemu-system-x86_64" + (".exe" if WINDOWS else "")
    for d in filter(None, [args.qemu_dir, r"C:\Program Files\qemu" if WINDOWS else None]):
        if os.path.exists(os.path.join(d, exe)):
            return os.path.join(d, exe)
    path = shutil.which("qemu-system-x86_64")
    if not path:
        die("qemu-system-x86_64 not found (use --qemu-dir)")
    return path


def qemu_command(args):
    vm = args.vmroot
    kernel = os.path.join(vm, "output", "ifs.bin")
    disk = os.path.join(vm, "output", "disk-qemu.vmdk")
    for f in (kernel, disk):
        if not os.path.exists(f):
            die(f"{f} is missing: run 'qv.py image' first")
    netdev = f"user,id=net0,hostfwd=udp:127.0.0.1:{BRIDGE_GUEST_PORT}-:{BRIDGE_GUEST_PORT}"
    # The NIC in PCI slot 0x17: the slot for which the image's pci_hw.cfg (made by
    # mkqnximage) has an interrupt entry; elsewhere the guest receives nothing.
    return [qemu_binary(args), "-accel", "tcg", "-cpu", "max", "-smp", "2", "-m", "1G",
            "-display", "none", "-no-reboot", "-kernel", kernel, "-serial", "file:serial.log",
            "-drive", f"file={disk},if=ide,id=drv0",
            "-netdev", netdev, "-device", "virtio-net-pci,netdev=net0,addr=0x17",
            "-object", "rng-builtin,id=rng0", "-device", "virtio-rng-pci,rng=rng0"]


def cmd_run(args):
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    rundir = os.path.join(ROOT, "results", "runs", stamp)
    os.makedirs(rundir)
    with open(os.path.join(rundir, "build.txt"), "w") as fh:
        fh.write(f"sources {source_commit()}\n")
    cmd = qemu_command(args)
    write_text(os.path.join(rundir, "qemu_command.txt"), " ".join(cmd) + "\n")

    sys.path.insert(0, HERE)
    import qv_host  # noqa: E402
    stop = threading.Event()
    host_log = os.path.join(rundir, "host_bridge.txt")
    peer = threading.Thread(target=qv_host.serve, daemon=True,
                            args=("127.0.0.1", BRIDGE_HOST_PORT, "127.0.0.1", BRIDGE_GUEST_PORT,
                                  host_log, stop))
    peer.start()

    say("starting QEMU")
    t0 = time.time()
    with open(os.path.join(rundir, "qemu.log"), "wb") as qlog:
        p = subprocess.Popen(cmd, cwd=rundir, stdout=qlog, stderr=subprocess.STDOUT)
        deadline = t0 + args.timeout * 60
        last = 0.0
        while p.poll() is None:
            time.sleep(2)
            if time.time() > deadline:
                p.kill()
                p.wait()
                break
            if time.time() - last > 30:
                last = time.time()
                begins = re.findall(r"QV (?:BEGIN|RUN) (\S+)",
                                    read_text(os.path.join(rundir, "serial.log")))
                say(f"running: {begins[-1] if begins else 'boot'}")
    status = (f"exit {p.returncode} after {time.time() - t0:.0f} s"
              if time.time() <= deadline else f"TIMEOUT after {args.timeout} min")
    stop.set()
    peer.join(5)
    write_text(os.path.join(rundir, "qemu_status.txt"), status + "\n")
    write_text(os.path.join(rundir, "qemu.log"), read_text(os.path.join(rundir, "qemu.log")))
    say(f"QEMU: {status}")
    report(rundir)
    return rundir


# ----------------------------------------------------------------- report
LINE = re.compile(r"QV (PASS|FAIL|LAT|SUMMARY|BEGIN|RUN|EXIT|GUEST|DUMP|STAT|GEN|BRIDGE|DEMO) ?(.*)")


def parse(text):
    r = {"pass": [], "fail": [], "lat": [], "summary": [], "exit": {}, "guest": [],
         "demo": [], "begun": []}
    for line in text.split("\n"):
        m = LINE.search(line)
        if not m:
            continue
        kind, rest = m.group(1), m.group(2).strip()
        if kind == "PASS":
            r["pass"].append(rest)
        elif kind == "FAIL":
            r["fail"].append(rest)
        elif kind == "LAT":
            r["lat"].append(rest)
        elif kind == "SUMMARY":
            r["summary"].append(rest)
        elif kind == "BEGIN":
            r["begun"].append(rest)
        elif kind == "EXIT":
            name, _, code = rest.partition(" ")
            r["exit"][name] = code
        elif kind == "GUEST":
            r["guest"].append(rest)
        elif kind in ("DUMP", "STAT", "GEN", "BRIDGE", "DEMO"):
            r["demo"].append(kind + " " + rest)
    return r


def md(text):
    return text.replace("|", "\\|")


def report(rundir=None):
    if rundir is None:
        runs = sorted(glob.glob(os.path.join(ROOT, "results", "runs", "*")))
        if not runs:
            die("no run in results/runs")
        rundir = runs[-1]
    serial = read_text(os.path.join(rundir, "serial.log"))
    d = parse(serial)
    host = read_text(os.path.join(rundir, "host_bridge.txt")).strip().split("\n")
    out = [f"# qvcan: run {os.path.basename(rundir)}", "",
           "Generated by `scripts/qv.py report` from the console log of the QEMU guest "
           "(`serial.log`).", "",
           "- " + read_text(os.path.join(rundir, "build.txt")).strip(),
           "- QEMU: " + read_text(os.path.join(rundir, "qemu_status.txt")).strip(), ""]
    uname = [g for g in d["guest"] if g.startswith("uname")]
    if uname:
        out += ["- guest: `" + md(uname[0][len("uname"):].strip()) + "`", ""]
    out += ["## Tests on the target", "", f"Summary lines: {'; '.join(d['summary']) or 'none'}", "",
            "| result | test | detail |", "|---|---|---|"]
    for p in d["pass"]:
        name, _, detail = p.partition(" ")
        out.append(f"| PASS | {md(name)} | {md(detail)} |")
    for f in d["fail"]:
        name, _, detail = f.partition(" ")
        out.append(f"| **FAIL** | {md(name)} | {md(detail)} |")
    done = {p.split(" ")[0].split(".")[0] for p in d["pass"] + d["fail"]}
    for b in d["begun"]:
        if b.split(".")[0] not in done:
            out.append(f"| **no result** | {md(b)} | the test started but printed no result |")
    out += ["", "## Latency and throughput (QEMU TCG: software CPU)", ""]
    out += [f"- `{md(x)}`" for x in d["lat"]] or ["- none"]
    out += ["", "## Programs", ""]
    out += [f"- `{k}` exit {v}" for k, v in d["exit"].items()] or ["- none"]
    out += ["", "## Demo (excerpt)", "", "```"]
    demo = d["demo"]
    out += demo[:60] + (["..."] + demo[-20:] if len(demo) > 80 else demo[60:])
    out += ["```", "", "## Host end of the bridge (`host_bridge.txt`)", "", "```"]
    out += host[-15:]
    out += ["```", ""]
    write_text(os.path.join(rundir, "SUMMARY.md"), "\n".join(out))
    say(f"report: {os.path.join(rundir, 'SUMMARY.md')}: {len(d['pass'])} PASS, "
        f"{len(d['fail'])} FAIL")


def cmd_report(args):
    report(args.rundir)


def cmd_all(args):
    args.arch = list(ARCHES)
    cmd_build(args)
    cmd_image(args)
    return cmd_run(args)


# ------------------------------------------------------------------ watch
def cmd_watch(args):
    """Unattended: run whenever the branch has a new commit of sources."""
    until = datetime.datetime.strptime(args.until, "%Y-%m-%d")
    say(f"watching origin/{args.branch} until {args.until}; close this window to stop")
    seen = set()  # source commits already run, and our own result commits
    while datetime.datetime.now() < until:
        git("fetch", "-q", "origin", args.branch, check=False)
        head = git("rev-parse", "FETCH_HEAD", check=False)
        subject = git("log", "-1", "--format=%s", "FETCH_HEAD", check=False)
        if head and head not in seen and not subject.startswith("results:"):
            # fast-forward only: the new commits sit on top of our result commits
            if subprocess.run(["git", "-C", ROOT, "merge", "-q", "--ff-only", "FETCH_HEAD"]).returncode:
                say(f"origin/{args.branch} is not a fast-forward of this clone; skipping it")
                seen.add(head)
                time.sleep(args.poll)
                continue
            say(f"new sources {head[:12]}: {subject}")
            try:
                rundir = cmd_all(args)
            except SystemExit as e:  # a failed build or image: record it, go on
                rundir = None
                say(f"run failed: {e.code}")
            seen.add(head)
            stamp = os.path.basename(rundir) if rundir else "failed"
            git("add", "-A", "results", check=False)
            git("commit", "-q", "-m", f"results: run {stamp} of {head[:12]}", check=False)
            for attempt in range(4):
                if subprocess.run(["git", "-C", ROOT, "push", "-q", "origin",
                                   f"HEAD:{args.branch}"]).returncode == 0:
                    break
                git("pull", "-q", "--rebase", "origin", args.branch, check=False)
                time.sleep(5 * (attempt + 1))
            seen.add(git("rev-parse", "HEAD", check=False))
            say("results pushed; waiting for new sources")
        time.sleep(args.poll)
    say("watch period over")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--vmroot", default=DEFAULT_VMROOT)
        p.add_argument("--qemu-dir", default=None)
        p.add_argument("--timeout", type=float, default=20, help="minutes")

    p = sub.add_parser("build")
    p.add_argument("--arch", type=lambda s: [a for a in s.split(",") if a], default=list(ARCHES))
    p.set_defaults(fn=cmd_build)
    p = sub.add_parser("image")
    common(p)
    p.set_defaults(fn=cmd_image)
    p = sub.add_parser("run")
    common(p)
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser("report")
    p.add_argument("rundir", nargs="?")
    p.set_defaults(fn=cmd_report)
    p = sub.add_parser("all")
    common(p)
    p.set_defaults(fn=cmd_all)
    p = sub.add_parser("watch")
    common(p)
    p.add_argument("--branch", required=True)
    p.add_argument("--until", default="2026-12-14")
    p.add_argument("--poll", type=float, default=60, help="seconds between checks")
    p.set_defaults(fn=cmd_watch)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
