#!/usr/bin/env python3
"""What the QNX headers declare with the feature macros of open62541.h.

    python tools/feature_macros.py [--arch x86_64,aarch64le] [--out FILE]

open62541.h defines _XOPEN_SOURCE 600, _DEFAULT_SOURCE, _BSD_SOURCE and
_GNU_SOURCE before its first system header. The QNX headers then declare only
what belongs to those standards unless _QNX_SOURCE is defined as well. Since
v1.4.12 open62541.h defines _QNX_SOURCE when the compiler predefines __QNX__
(v1.4.10 did not, and an earlier version of this port added it). This script
compiles one small file per name with qcc, once with the macros of the
release ("release") and once without its QNX block ("noqnx", as up to v1.4.11),
and prints whether the name is declared. It also prints what qcc predefines, a
few values, and _QNX_SOURCE and UA_FLOAT_IEEE754 as the release header and
the port header decide them.

Run the QNX SDP environment script first; the output also goes to
results/feature_macros.txt. Nothing is run on the target.
SPDX-License-Identifier: Apache-2.0
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VARIANT = {"x86_64": "gcc_ntox86_64", "aarch64le": "gcc_ntoaarch64le"}

# the feature macros of open62541.h v1.4.22, in its order (the release puts
# each of the four under #ifndef, which changes nothing in these probes)
NOQNX_MACROS = """#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE
"""
RELEASE_MACROS = "#if defined(__QNX__)\n# define _QNX_SOURCE 1\n#endif\n" + NOQNX_MACROS

HEADERS = """#include <limits.h>
#include <poll.h>
#include <dirent.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <ifaddrs.h>
"""

# (name, kind, what uses it)
NAMES = [
    ("ppoll", "function", "the event loop of the port (eventloop-ppoll)"),
    ("scandir", "function", "the certificate folders (pki-folders)"),
    ("alphasort", "function", "the certificate folders (pki-folders)"),
    ("socketpair", "function", "the interrupt manager without pipe() (interrupt-selfpipe)"),
    ("getifaddrs", "function", "the release (PubSub interfaces)"),
    ("PATH_MAX", "macro", "the certificate folders (pki-folders)"),
    ("MSG_NOSIGNAL", "macro", "the release: no SIGPIPE when a peer is gone"),
    ("MSG_DONTWAIT", "macro", "the release"),
    ("SO_NOSIGPIPE", "macro", "the release: no SIGPIPE when a peer is gone"),
    ("SO_REUSEPORT", "macro", "the release"),
    ("IP_MULTICAST_IF", "macro", "the release (PubSub)"),
]
VALUES = ["FD_SETSIZE", "PATH_MAX", "MSG_NOSIGNAL", "SO_NOSIGPIPE"]
PREDEFINED = ["_QNX_SOURCE", "__QNX__", "__QNXNTO__", "__STDC_IEC_559__", "__GCC_IEC_559",
              "__STDC_VERSION__"]


def qcc():
    path = shutil.which("qcc")
    if not path:
        sys.exit("feature_macros.py: qcc not found; run the QNX SDP environment script first")
    return path


def compiles(cc, arch, source, tmp):
    src = os.path.join(tmp, "probe.c")
    with open(src, "w", newline="\n") as fh:
        fh.write(source)
    p = subprocess.run([cc, "-V" + VARIANT[arch], "-std=gnu99", "-c", src, "-o",
                        os.path.join(tmp, "probe.o")], capture_output=True)
    return p.returncode == 0


def macros(cc, arch, source, tmp, include_dirs=()):
    src = os.path.join(tmp, "macros.c")
    with open(src, "w", newline="\n") as fh:
        fh.write(source)
    cmd = [cc, "-V" + VARIANT[arch], "-std=gnu99", "-E", "-dM"]
    for d in include_dirs:
        cmd += ["-I", d]
    p = subprocess.run(cmd + [src], capture_output=True)
    if p.returncode != 0:
        return None
    out = {}
    for line in p.stdout.decode("utf-8", "replace").splitlines():
        m = re.match(r"#define (\w+)(?: (.*))?$", line.strip())
        if m:
            out[m.group(1)] = (m.group(2) or "").strip()
    return out


def expanded(cc, arch, source, names, tmp):
    """The value each macro expands to (the name itself if it is no macro)."""
    src = os.path.join(tmp, "values.c")
    with open(src, "w", newline="\n") as fh:
        fh.write(source + "".join(f"o6q_value_{n} = {n};\n" for n in names))
    # -Wp,-P: no line markers, so that an expansion stays on its line (qcc has
    # an option -P of its own)
    p = subprocess.run([cc, "-V" + VARIANT[arch], "-std=gnu99", "-E", "-Wp,-P", src],
                       capture_output=True)
    defined = macros(cc, arch, source, tmp) or {}
    out = {}
    for line in p.stdout.decode("utf-8", "replace").splitlines():
        m = re.match(r"\s*o6q_value_(\w+) = (.*);\s*$", line)
        if m:
            value = " ".join(m.group(2).split())
            if value == m.group(1):  # not a macro, or one that names itself (an enum)
                value = "defined as itself" if m.group(1) in defined else "not defined"
            out[m.group(1)] = value
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--arch", default="x86_64,aarch64le")
    ap.add_argument("--out", default=os.path.join(ROOT, "results", "feature_macros.txt"))
    args = ap.parse_args()
    cc = qcc()
    lines = []

    def say(text):
        print(text, flush=True)
        lines.append(text)

    with tempfile.TemporaryDirectory() as tmp:
        for arch in [a.strip() for a in args.arch.split(",") if a.strip()]:
            pre = macros(cc, arch, "int o6q;\n", tmp) or {}
            for name in PREDEFINED:
                say(f"O6Q FEATURE {arch} predefined {name} " +
                    ((pre[name] or "defined") if name in pre else "not defined"))
            for variant, block in (("noqnx", NOQNX_MACROS), ("release", RELEASE_MACROS)):
                for name, kind, use in NAMES:
                    if kind == "function":
                        body = f"int main(void) {{ return &{name} != 0; }}\n"
                    else:
                        body = (f"#ifndef {name}\n#error {name} not declared\n#endif\n"
                                "int main(void) { return 0; }\n")
                    ok = compiles(cc, arch, block + HEADERS + body, tmp)
                    say(f"O6Q FEATURE {arch} {variant} {name} "
                        f"{'declared' if ok else 'NOT DECLARED'} ({use})")
                vals = expanded(cc, arch, block + HEADERS, VALUES, tmp)
                for name in VALUES:
                    say(f"O6Q FEATURE {arch} {variant} value {name} {vals.get(name, '-')}")
            for variant, sub in (("release", "upstream"), ("port", "")):
                inc = os.path.join(ROOT, "third_party", sub) if sub else \
                    os.path.join(ROOT, "third_party")
                if not os.path.exists(os.path.join(inc, "open62541.h")):
                    say(f"O6Q FEATURE {arch} {variant} header not found: run scripts/port.py")
                    continue
                vals = macros(cc, arch, '#include "open62541.h"\n', tmp, [inc])
                if vals is None:
                    say(f"O6Q FEATURE {arch} {variant} header does not preprocess")
                    continue
                for name in ("UA_FLOAT_IEEE754", "UA_FLOAT_LITTLE_ENDIAN",
                             "UA_BINARY_OVERLAYABLE_FLOAT", "_QNX_SOURCE", "__STDC_IEC_559__"):
                    say(f"O6Q FEATURE {arch} {variant} open62541.h {name} "
                        f"{vals.get(name, 'not defined') or 'defined'}")

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print("feature_macros.py: " + os.path.relpath(args.out, ROOT))


if __name__ == "__main__":
    main()
