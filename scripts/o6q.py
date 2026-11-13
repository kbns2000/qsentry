#!/usr/bin/env python3
"""Build the open62541 QNX port, put it into QEMU images, run it, report.

    python scripts/o6q.py build  [--arch x86_64,aarch64le]
    python scripts/o6q.py image  [--arch ...] [--interop] [--vmroot DIR]
    python scripts/o6q.py run    [--arch ...] [--interop] [--vmroot DIR] [--timeout MIN]
    python scripts/o6q.py report [RUN_DIR]
    python scripts/o6q.py all    [--arch ...] [--interop]

Run the QNX SDP 8.0 environment script first (qnxsdp-env.bat, or
"source qnxsdp-env.sh"), so that QNX_HOST and QNX_TARGET are set.

build   scripts/port.py, then CMake and Ninja for each CPU in build/<arch>
        (cmake/qnx800.toolchain.cmake). The compiler output is kept in
        results/build_<arch>.log; the programs are copied to out/<arch>/.
image   one QEMU image per CPU with mkqnximage, in <vmroot>/<arch>. The image
        holds the programs, vm/run.sh (started at boot), the OpenSSL library
        of the SDP and, with --interop, a certificate of the host client in
        the trust directory of the demo server.
run     boots the images in QEMU (software emulation, both CPUs at the same
        time) and waits until the guests shut down. The console of each guest
        goes to results/runs/<time>/<arch>/serial.log. With --interop the
        x86_64 guest's port 4840 is forwarded to 127.0.0.1:48400 and
        scripts/o6q_client.py (python asyncua) talks to the demo server.
report  summarizes the "O6Q ..." lines of a run in SUMMARY.md (default: the
        newest run).

Standard library only; the interop client needs the asyncua package.
SPDX-License-Identifier: Apache-2.0
"""
import argparse
import datetime
import glob
import os
import re
import secrets
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WINDOWS = os.name == "nt"
ARCHES = ("x86_64", "aarch64le")
PROGRAMS = ("o6q_check", "o6q_check_upstream", "o6q_envprobe", "o6q_server")
INTEROP_PORT = 48400  # host side of the forwarded port 4840 of the x86_64 guest
DEMO_USER = "operator"
DEFAULT_VMROOT = os.path.join(ROOT, "out", "vm")

sys.path.insert(0, HERE)
import port as portscript  # noqa: E402  (scripts/port.py: the list of changes)


def die(msg):
    sys.exit("o6q.py: " + msg)


def say(msg):
    print("o6q.py: " + msg, flush=True)


def qnx_env():
    host, target = os.environ.get("QNX_HOST"), os.environ.get("QNX_TARGET")
    if not host or not target:
        die("QNX_HOST and QNX_TARGET are not set. Run the SDP environment script first "
            "(qnxsdp-env.bat, or: source qnxsdp-env.sh).")
    return host, target


def tool(name):
    """A program from PATH (on Windows also .bat/.cmd/.exe)."""
    path = shutil.which(name)
    if not path:
        die(f"{name} not found in PATH")
    if WINDOWS and path.lower().endswith((".bat", ".cmd")):
        return [os.environ.get("COMSPEC", r"C:\Windows\System32\cmd.exe"), "/c", path]
    return [path]


HOME = os.path.expanduser("~")


def scrub(text):
    """The home directory as ~ in what goes into results/ (no user names there)."""
    for home in {HOME, HOME.replace("\\", "/")}:
        if len(home) > 3:
            text = re.sub(re.escape(home), "~", text, flags=re.I)
    return text


def scrub_file(path):
    if os.path.exists(path):
        text = read_text(path)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(scrub(text))


def run(cmd, cwd=None, log=None):
    """Runs cmd, shows its output and appends it to log. Stops on failure."""
    print("$ " + " ".join(cmd), flush=True)
    with subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as p:
        out = open(log, "a", encoding="utf-8", newline="\n") if log else None
        try:
            for line in p.stdout:
                text = line.decode("utf-8", "replace").replace("\r\n", "\n")
                sys.stdout.write(text)
                if out:
                    out.write(scrub(text))
        finally:
            if out:
                out.close()
    if p.returncode != 0:
        die(f"failed ({p.returncode}): {' '.join(cmd)}")


def arches(text):
    names = [a.strip() for a in text.split(",") if a.strip()]
    for a in names:
        if a not in ARCHES:
            die(f"unknown CPU {a}; known: {', '.join(ARCHES)}")
    return names


# ------------------------------------------------------------------- build
def source_commit():
    """The git commit of the sources, with "+changes" when files outside
    results/ (which this script writes) differ from it."""
    try:
        head = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short=12", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
        changed = subprocess.run(["git", "-C", ROOT, "status", "--porcelain",
                                  "--untracked-files=no", "--", ".", ":(exclude)results"],
                                 capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"
    return head + ("+changes" if changed else "")


def cmd_build(args):
    qnx_env()
    commit = source_commit()
    say(f"sources: commit {commit}")
    run([sys.executable, os.path.join(ROOT, "scripts", "port.py")])
    os.makedirs(os.path.join(ROOT, "results"), exist_ok=True)
    for arch in args.arch:
        bdir = os.path.join(ROOT, "build", arch)
        log = os.path.join(ROOT, "results", f"build_{arch}.log")
        if os.path.exists(log):
            os.remove(log)
        run(tool("cmake") + ["-G", "Ninja", "-S", ROOT, "-B", bdir,
                             "-DCMAKE_TOOLCHAIN_FILE=" +
                             os.path.join(ROOT, "cmake", "qnx800.toolchain.cmake"),
                             "-DQNX_ARCH=" + arch, "-DCMAKE_BUILD_TYPE=Release",
                             "-DO6Q_SOURCE_COMMIT=" + commit], log=log)
        # -v: every compiler command line goes into the log, with its output
        run(tool("cmake") + ["--build", bdir, "--clean-first", "-v"], log=log)
        dest = os.path.join(ROOT, "out", arch)
        os.makedirs(dest, exist_ok=True)
        for prog in PROGRAMS:
            src = os.path.join(bdir, prog)
            if not os.path.exists(src):
                die(f"{src} was not built")
            shutil.copy2(src, os.path.join(dest, prog))
        warnings = count_warnings(log)
        say(f"{arch}: {len(PROGRAMS)} programs in {dest}; compiler warnings: " +
            ", ".join(f"{k} {v}" for k, v in warnings.items()))


def count_warnings(log):
    """Warnings per compiled file of interest, from a build log."""
    counts = {"port library": 0, "upstream library": 0, "programs": 0}
    current = None
    with open(log, "rb") as fh:
        for raw in fh:
            line = raw.decode("utf-8", "replace")
            if " -c " in line and ("qcc" in line or "gcc" in line or "cc " in line):
                if "upstream" in line and "open62541.c" in line:
                    current = "upstream library"
                elif "open62541.c" in line:
                    current = "port library"
                else:
                    current = "programs"
            elif "warning:" in line and current:
                counts[current] += 1
    return counts


# ------------------------------------------------------------------- image
def find_libcrypto(target, arch):
    for sub in ("usr/lib", "lib"):
        for name in ("libcrypto.so.3", "libcrypto.so"):
            path = os.path.join(target, arch, sub, name)
            if os.path.exists(path):
                return os.path.realpath(path)
    return None


def host_pki(vmroot):
    return os.path.join(vmroot, "pki")


def make_host_identity(vmroot):
    """The certificate and key of the host client, and the demo password."""
    pki = host_pki(vmroot)
    os.makedirs(pki, exist_ok=True)
    cert, key = os.path.join(pki, "host_cert.der"), os.path.join(pki, "host_key.pem")
    if not (os.path.exists(cert) and os.path.exists(key)):
        run([sys.executable, os.path.join(ROOT, "scripts", "o6q_client.py"),
             "--make-cert", cert, key])
    pw_file = os.path.join(pki, "demo_password.txt")
    if not os.path.exists(pw_file):
        with open(pw_file, "w") as fh:
            fh.write(secrets.token_urlsafe(12) + "\n")
    with open(pw_file) as fh:
        password = fh.read().strip()
    return cert, key, password


def cmd_image(args):
    host, target = qnx_env()
    made, failed = [], []
    for arch in args.arch:
        vm = os.path.join(args.vmroot, arch)
        snippets = os.path.join(vm, "local", "snippets")
        payload = os.path.join(vm, "payload")
        shutil.rmtree(payload, ignore_errors=True)
        os.makedirs(snippets, exist_ok=True)
        os.makedirs(payload)

        files = []  # (path in /data/o6q, host path, permissions)
        for prog in PROGRAMS:
            src = os.path.join(ROOT, "out", arch, prog)
            if not os.path.exists(src):
                die(f"{src} is missing: run 'o6q.py build' first")
            files.append((prog, src, "0755"))
        # vm/run.sh with LF line ends, whatever git did on checkout
        with open(os.path.join(ROOT, "vm", "run.sh"), "rb") as fh:
            script = fh.read().replace(b"\r\n", b"\n")
        with open(os.path.join(payload, "run.sh"), "wb") as fh:
            fh.write(script)
        files.append(("run.sh", os.path.join(payload, "run.sh"), "0755"))
        lib = find_libcrypto(target, arch)
        if lib:
            files.append(("lib/libcrypto.so.3", lib, "0644"))
        else:
            say(f"warning: no libcrypto.so.3 below {target}/{arch}; the image must have one")
        if args.interop and arch == "x86_64":
            cert, _key, password = make_host_identity(args.vmroot)
            files.append(("pki/trusted/host_client.der", cert, "0644"))
            conf = os.path.join(payload, "demo.conf")
            with open(conf, "w", newline="\n") as fh:
                fh.write(f"O6Q_DEMO_USER={DEMO_USER}\nO6Q_DEMO_PASS={password}\n")
            files.append(("demo.conf", conf, "0600"))

        with open(os.path.join(snippets, "data_files.custom"), "w", newline="\n") as fh:
            for dest, src, perms in files:
                fh.write(f"[uid=0 gid=0 perms={perms}] o6q/{dest}=" +
                         os.path.abspath(src).replace("\\", "/") + "\n")
        shutil.copyfile(os.path.join(ROOT, "vm", "post_start.custom"),
                        os.path.join(snippets, "post_start.custom"))

        cmd = tool("mkqnximage") + ["--noprompt", "--force", "--type=qemu", "--arch=" + arch,
                                    "--hostname=o6q", "--ssh-ident=none", "--build"]
        cmd += args.mkqnximage_option
        try:
            run(cmd, cwd=vm)
        except SystemExit as e:
            # e.g. an SDP without the QEMU board support package for this CPU
            # (startup-qemu-virt, devb-virtio): the other CPUs still get theirs,
            # and 'run' skips a CPU without an image
            ifs = os.path.join(vm, "output", "ifs.bin")
            if os.path.exists(ifs):
                os.remove(ifs)  # no stale image of an earlier build
            failed.append(arch)
            say(f"{arch}: no image ({e.code})")
            continue
        made.append(arch)
        say(f"{arch}: image in {os.path.join(vm, 'output')}")
    if not made:
        die("no image was made")
    if failed:
        say("no image for " + ", ".join(failed) + "; 'run' skips " +
            ("it" if len(failed) == 1 else "them"))


# --------------------------------------------------------------------- run
def qemu_binary(args, arch):
    name = "qemu-system-x86_64" if arch == "x86_64" else "qemu-system-aarch64"
    exe = name + (".exe" if WINDOWS else "")
    for d in filter(None, [args.qemu_dir, os.environ.get("O6Q_QEMU_DIR"),
                           r"C:\Program Files\qemu" if WINDOWS else None]):
        if os.path.exists(os.path.join(d, exe)):
            return os.path.join(d, exe)
    path = shutil.which(name)
    if not path:
        die(f"{name} not found (use --qemu-dir)")
    return path


def aarch64_machine(qemu):
    """virt-4.2 as mkqnximage uses it, if this QEMU still has it."""
    try:
        out = subprocess.run([qemu, "-machine", "help"], capture_output=True, timeout=60).stdout
        if re.search(rb"^virt-4\.2\s", out, re.M):
            return "virt-4.2"
    except (OSError, subprocess.SubprocessError):
        pass
    return "virt,gic-version=2"


def qemu_command(args, arch, vm, interop):
    qemu = qemu_binary(args, arch)
    kernel = os.path.join(vm, "output", "ifs.bin")
    disk = os.path.join(vm, "output", "disk-qemu.vmdk")
    for f in (kernel, disk):
        if not os.path.exists(f):
            die(f"{f} is missing: run 'o6q.py image' first")
    common = ["-smp", "2", "-m", "1G", "-display", "none", "-no-reboot",
              "-kernel", kernel, "-serial", "file:serial.log"]
    if arch == "x86_64":
        nic = "user,model=virtio-net-pci"
        if interop:
            nic += f",hostfwd=tcp:127.0.0.1:{INTEROP_PORT}-:4840"
        return [qemu, "-accel", "tcg", "-cpu", "max"] + common + [
            "-drive", f"file={disk},if=ide,id=drv0", "-nic", nic,
            # as mkqnximage: an entropy source for the random service, which
            # OpenSSL and the security suite depend on (rng-builtin: Windows
            # has no /dev/urandom for rng-random)
            "-object", "rng-builtin,id=rng0", "-device", "virtio-rng-pci,rng=rng0"]
    return [qemu, "-machine", aarch64_machine(qemu), "-cpu", "cortex-a57"] + common + [
        "-drive", f"file={disk},if=none,id=drv0", "-device", "virtio-blk-device,drive=drv0",
        "-netdev", "user,id=net0", "-device", "virtio-net-device,netdev=net0",
        # as mkqnximage: the third virtio-mmio device (0xa003a00) is the entropy
        # source of 'random -l devr-virtio.so:mem=0xa003a00' in the aarch64 image
        "-object", "rng-builtin,id=rng0", "-device", "virtio-rng-device,rng=rng0"]


def read_text(path):
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace").replace("\r", "")
    except OSError:
        return ""


def run_interop(args, rundir):
    cert, key, password = make_host_identity(args.vmroot)
    out = os.path.join(rundir, "x86_64", "interop.txt")
    cmd = [sys.executable, os.path.join(ROOT, "scripts", "o6q_client.py"),
           f"opc.tcp://127.0.0.1:{INTEROP_PORT}", "--cert", cert, "--key", key,
           "--user", f"{DEMO_USER}:{password}", "--seconds", "15"]
    say("interop: " + " ".join(cmd[:3]) + " ...")
    with open(out, "wb") as fh:
        try:
            p = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, timeout=600)
            code = p.returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
    with open(out, "ab") as fh:
        fh.write(f"O6Q HOST EXIT {code}\n".encode())
    scrub_file(out)
    say(f"interop client finished ({code}): {out}")


def cmd_run(args):
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    rundir = os.path.join(ROOT, "results", "runs", stamp)
    procs = {}
    skipped = {}
    for arch in args.arch:
        vm = os.path.join(args.vmroot, arch)
        if not os.path.exists(os.path.join(vm, "output", "ifs.bin")):
            # e.g. an SDP without the QEMU board support for this CPU
            skipped[arch] = "not run: no image (o6q.py image)"
            say(f"{arch}: no image in {vm}, not run")
            continue
        adir = os.path.join(rundir, arch)
        os.makedirs(adir)
        interop = args.interop and arch == "x86_64"
        cmd = qemu_command(args, arch, vm, interop)
        with open(os.path.join(adir, "qemu_command.txt"), "w") as fh:
            fh.write(scrub(" ".join(cmd)) + "\n")
        qlog = open(os.path.join(adir, "qemu.log"), "wb")
        say(f"{arch}: starting QEMU")
        procs[arch] = (subprocess.Popen(cmd, cwd=adir, stdout=qlog, stderr=subprocess.STDOUT),
                       qlog, time.time())

    if not procs:
        die("no image to run: run 'o6q.py image' first")
    deadline = time.time() + args.timeout * 60
    interop_done = not args.interop or "x86_64" not in procs
    status = {}
    last_note = 0.0
    while len(status) < len(procs):
        time.sleep(2)
        for arch, (p, qlog, t0) in procs.items():
            if arch in status:
                continue
            if p.poll() is not None:
                status[arch] = f"exit {p.returncode} after {time.time() - t0:.0f} s"
                qlog.close()
                say(f"{arch}: QEMU ended ({status[arch]})")
            elif time.time() > deadline:
                p.kill()
                p.wait()
                qlog.close()
                status[arch] = f"TIMEOUT after {args.timeout} min"
                say(f"{arch}: {status[arch]}, QEMU stopped")
        if not interop_done:
            serial = read_text(os.path.join(rundir, "x86_64", "serial.log"))
            if "O6Q DEMO READY" in serial:
                run_interop(args, rundir)
                interop_done = True
            elif "x86_64" in status:
                interop_done = True
        if time.time() - last_note > 60:
            last_note = time.time()
            for arch in procs:
                if arch not in status:
                    serial = read_text(os.path.join(rundir, arch, "serial.log"))
                    runs = re.findall(r"O6Q RUN (\S+)", serial)
                    say(f"{arch}: running {runs[-1] if runs else 'boot'}")
    with open(os.path.join(rundir, "qemu_status.txt"), "w") as fh:
        for arch, s in list(status.items()) + list(skipped.items()):
            fh.write(f"{arch} {s}\n")
    for arch in procs:
        scrub_file(os.path.join(rundir, arch, "qemu.log"))
    report(rundir)


# ------------------------------------------------------------------ report
LINE = re.compile(r"O6Q (BUILD|BEGIN|PASS|FAIL|SKIP|LAT|INFO|LOG|SUMMARY|ABORT|ENV|RUN|EXIT|"
                  r"GUEST|DEMO|HOST) ?(.*)")
TEST_NAME = re.compile(r"\b([a-z0-9_]+(?:\.[A-Za-z0-9_]+)+)\b")


def parse(text):
    r = {"build": [], "results": {}, "summary": {}, "lat": [], "env": {}, "exit": {},
         "abort": [], "fail": [], "demo": [], "guest": [], "host": [], "log": 0, "begun": []}
    for line in text.split("\n"):
        m = LINE.search(line)
        if not m:
            continue
        kind, rest = m.group(1), m.group(2).strip()
        if kind == "BUILD":
            if rest not in r["build"]:
                r["build"].append(rest)
        elif kind == "BEGIN":
            suite = rest.partition("suite=")[2].split(" ")[0]
            if suite and suite not in r["begun"]:
                r["begun"].append(suite)
        elif kind in ("PASS", "FAIL", "SKIP"):
            name, _, detail = rest.partition(" ")
            r["results"][name] = (kind, detail)
            if kind == "FAIL":
                r["fail"].append(f"{name} {detail}")
        elif kind == "SUMMARY":
            fields = dict(kv.split("=", 1) for kv in rest.split() if "=" in kv)
            if "suite" in fields:
                r["summary"][fields["suite"]] = fields
        elif kind == "LAT":
            r["lat"].append(rest)
        elif kind == "ENV":
            key, _, value = rest.partition("=")
            r["env"][key] = value
        elif kind == "EXIT":
            parts = rest.split()
            if len(parts) >= 2:
                r["exit"][parts[0]] = parts[1]
        elif kind == "ABORT":
            r["abort"].append(rest)
        elif kind == "DEMO":
            r["demo"].append(rest)
        elif kind == "GUEST":
            r["guest"].append(rest)
        elif kind == "HOST":
            r["host"].append(rest)
        elif kind == "LOG":
            r["log"] += 1
    return r


def lookup(results, test, variant):
    """Result of "suite.test" in the port or the upstream program."""
    suite, _, rest = test.partition(".")
    name = f"{suite}@upstream.{rest}" if variant == "upstream" else test
    if name in results:
        return results[name][0]
    # a phase name that is part of a longer test name
    hits = [v[0] for k, v in results.items() if k.startswith(name + ".")]
    if hits:
        return "FAIL" if "FAIL" in hits else ("PASS" if "PASS" in hits else hits[0])
    return "-"


def md_escape(text):
    return text.replace("|", "\\|")


def report(rundir=None):
    if rundir is None:
        runs = sorted(glob.glob(os.path.join(ROOT, "results", "runs", "*")))
        if not runs:
            die("no run in results/runs")
        rundir = runs[-1]
    archs = [a for a in ARCHES if os.path.isdir(os.path.join(rundir, a))]
    data = {a: parse(read_text(os.path.join(rundir, a, "serial.log"))) for a in archs}
    interop = parse(read_text(os.path.join(rundir, "x86_64", "interop.txt")))
    linux_env = parse(read_text(os.path.join(ROOT, "results", "envprobe_linux.txt")))["env"]
    status = read_text(os.path.join(rundir, "qemu_status.txt")).strip()

    md = [f"# open62541-qnx: run {os.path.basename(rundir)}", ""]
    md += ["Generated by `scripts/o6q.py report` from the console logs of the QEMU guests "
           "(`<arch>/serial.log`).", ""]
    if status:
        md += ["QEMU: " + "; ".join(status.split("\n")), ""]
    for a in archs:
        for b in data[a]["build"]:
            md.append(f"- `{a}`: {b}")
    md.append("")

    md += ["## Suites", "",
           "| CPU | suite | port: pass / fail / skip | upstream release: pass / fail / skip |",
           "|---|---|---|---|"]
    for a in archs:
        suites = []
        for s in data[a]["begun"] + list(data[a]["summary"]):
            if s.split("@")[0] not in suites:
                suites.append(s.split("@")[0])
        for s in suites:
            cells = []
            for name in (s, s + "@upstream"):
                f = data[a]["summary"].get(name)
                if f:
                    cells.append(f"{f.get('pass')} / {f.get('fail')} / {f.get('skip')}")
                elif name in data[a]["begun"]:
                    cells.append("**aborted** (no summary)")
                else:
                    cells.append("-")
            md.append(f"| {a} | {s} | {cells[0]} | {cells[1]} |")
    md.append("")

    md += ["## The changes on the target", "",
           "Each change names the tests that show it (`# test:` in `patches/*.change`). "
           "Expected: PASS with the port, FAIL with the unchanged release.", ""]
    header = "| change | test |" + "".join(f" {a} port | {a} upstream |" for a in archs)
    md += [header, "|---|---|" + "---|---|" * len(archs)]
    for c in portscript.load_series():
        tests = TEST_NAME.findall(c.meta["test"]) or [c.meta["test"]]
        for t in tests:
            row = f"| {c.meta['id']} | {md_escape(t)} |"
            for a in archs:
                res = data[a]["results"]
                row += f" {lookup(res, t, 'port')} | {lookup(res, t, 'upstream')} |"
            md.append(row)
    md.append("")

    md += ["## Failures and aborts", ""]
    any_fail = False
    for a in archs:
        for f in data[a]["fail"] + [f"ABORT {x}" for x in data[a]["abort"]]:
            md.append(f"- `{a}` {md_escape(f)}")
            any_fail = True
        bad = {k: v for k, v in data[a]["exit"].items() if v != "0"}
        for k, v in bad.items():
            md.append(f"- `{a}` program {k} exit status {v}")
            any_fail = True
    if not any_fail:
        md.append("none")
    md.append("")

    md += ["## Latencies", "", "| CPU | what | values |", "|---|---|---|"]
    for a in archs:
        for line in data[a]["lat"]:
            what, _, values = line.partition(" ")
            md.append(f"| {a} | {what} | {values} |")
    md.append("")

    if interop["host"] or interop["results"]:
        md += ["## Host client (python asyncua) against o6q_server on QNX", ""]
        for name, (kind, detail) in interop["results"].items():
            md.append(f"- {kind} {name} {md_escape(detail)}")
        for line in interop["host"]:
            md.append(f"- {md_escape(line)}")
        md.append("")
    demo = [d for a in archs for d in data[a]["demo"]]
    if demo:
        md += ["## Demo server", ""] + [f"- {md_escape(d)}" for d in demo] + [""]

    keys = []
    for env in [linux_env] + [data[a]["env"] for a in archs]:
        for k in env:
            if k not in keys:
                keys.append(k)
    if keys:
        md += ["## Operating system behaviour (`o6q_envprobe`)", "",
               "| key | Linux (reference) |" + "".join(f" QNX {a} |" for a in archs),
               "|---|---|" + "---|" * len(archs)]
        for k in keys:
            row = f"| {k} | {md_escape(linux_env.get(k, '-'))} |"
            for a in archs:
                row += f" {md_escape(data[a]['env'].get(k, '-'))} |"
            md.append(row)
        md.append("")

    out = os.path.join(rundir, "SUMMARY.md")
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(md))
    for a in archs:
        p = sum(int(f.get("pass", 0)) for s, f in data[a]["summary"].items() if "@" not in s)
        fl = sum(int(f.get("fail", 0)) for s, f in data[a]["summary"].items() if "@" not in s)
        say(f"{a}: port {p} passed, {fl} failed; exits {data[a]['exit']}")
    say("summary: " + out)
    return out


def cmd_report(args):
    report(args.rundir)


def cmd_all(args):
    cmd_build(args)
    cmd_image(args)
    cmd_run(args)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="command", required=True)

    def common(p, image=False, run_=False):
        p.add_argument("--arch", type=arches, default=list(ARCHES),
                       help="comma-separated: x86_64,aarch64le (default: both)")
        if image or run_:
            p.add_argument("--vmroot", default=DEFAULT_VMROOT,
                           help="directory of the QEMU images (default: out/vm)")
            p.add_argument("--interop", action="store_true",
                           help="x86_64: also run the demo server and the host client")
        if image:
            p.add_argument("--mkqnximage-option", action="append", default=[],
                           help="extra option for mkqnximage (repeatable)")
        if run_:
            p.add_argument("--qemu-dir", help="directory of qemu-system-*")
            p.add_argument("--timeout", type=float, default=150.0,
                           help="minutes until a guest is stopped (default 150)")

    common(sub.add_parser("build", help="port and cross-compile"))
    common(sub.add_parser("image", help="QEMU images with mkqnximage"), image=True)
    common(sub.add_parser("run", help="boot the images, collect the logs"), run_=True)
    p = sub.add_parser("report", help="summarize a run")
    p.add_argument("rundir", nargs="?")
    common(sub.add_parser("all", help="build, image, run"), image=True, run_=True)
    args = ap.parse_args()
    # everything this script prints also goes to results/o6q_<command>.log
    os.makedirs(os.path.join(ROOT, "results"), exist_ok=True)
    log = open(os.path.join(ROOT, "results", f"o6q_{args.command}.log"), "w",
               encoding="utf-8", errors="replace")
    sys.stdout, sys.stderr = Tee(sys.stdout, log), Tee(sys.stderr, log)
    print(f"o6q.py {' '.join(sys.argv[1:])} at {datetime.datetime.now():%Y-%m-%d %H:%M:%S}",
          flush=True)
    {"build": cmd_build, "image": cmd_image, "run": cmd_run, "report": cmd_report,
     "all": cmd_all}[args.command](args)


class Tee:
    """Writes to a stream and to a log file."""

    def __init__(self, stream, log):
        self.stream, self.log = stream, log

    def write(self, text):
        self.stream.write(text)
        self.log.write(scrub(text))
        self.log.flush()
        return len(text)

    def flush(self):
        self.stream.flush()
        self.log.flush()


if __name__ == "__main__":
    main()
