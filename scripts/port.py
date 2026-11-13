#!/usr/bin/env python3
"""Fetch the official open62541 single-file release and apply the QNX 8.0 port.

    python scripts/port.py            fetch, check, port
    python scripts/port.py --list     print the changes of the port

Output (nothing of it is committed to this repository):

    third_party/open62541.{c,h}             the port
    third_party/upstream/open62541.{c,h}    the release as it is, with the same
                                            build configuration and no change
                                            in behaviour (for comparison runs)

The release files are downloaded and checked against pinned SHA-256 hashes.
The port then consists of

  1. the build configuration (CONFIG below): what the CMake options of
     upstream would select, expressed on the pre-configured release files;
  2. the changes in patches/, in the order of patches/series.

Every piece of text that is replaced must be found exactly once, otherwise the
script stops. A different upstream version therefore fails loudly instead of
producing a half-ported build.

Standard library only. SPDX-License-Identifier: Apache-2.0
"""
import hashlib
import os
import sys
import urllib.request

VERSION = "v1.4.22"  # the newest release of the 1.4 line (2026-10-30)
BASE = f"https://github.com/open62541/open62541/releases/download/{VERSION}/"
FILES = {
    "open62541.c": "763069e2e65242b157f07fbc423c87d1af3d670ff6af409b601beb962843e499",
    "open62541.h": "47b06f26b44617eb059d86d9c9f53438803e782733e80e096c86622883ff759b",
}

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PATCHES = os.path.join(ROOT, "patches")

# ---------------------------------------------------------------- configuration
# (file, old, new, why). The release is a snapshot of one CMake configuration
# of upstream; these are the options that differ for QNX.
CONFIG = [
    ("open62541.h", "#define UA_ENABLE_ENCRYPTION_MBEDTLS\n",
     "/* #undef UA_ENABLE_ENCRYPTION_MBEDTLS */ /* qnx: SDP 8.0 has no Mbed TLS */\n",
     "The release is configured for Mbed TLS, which QNX SDP 8.0 does not ship."),
    ("open62541.h", "/* #undef UA_ENABLE_ENCRYPTION_OPENSSL */\n",
     "#define UA_ENABLE_ENCRYPTION_OPENSSL /* qnx: the OpenSSL 3 of SDP 8.0 */\n",
     "QNX SDP 8.0 ships OpenSSL 3 for every target CPU and the release already contains "
     "the OpenSSL backend (upstream: -DUA_ENABLE_ENCRYPTION=OPENSSL)."),
    ("open62541.h", "#define UA_ENABLE_PUBSUB_ENCRYPTION\n",
     "/* #undef UA_ENABLE_PUBSUB_ENCRYPTION */ /* qnx: upstream has it for Mbed TLS only */\n",
     "PubSub message encryption exists upstream only on Mbed TLS "
     "(upstream: -DUA_ENABLE_PUBSUB_ENCRYPTION=OFF)."),
    # Not an option: the single file with OpenSSL does not compile without it.
    ("open62541.c",
     'static const unsigned char openssl_PEM_PRE[26] = "-----END CERTIFICATE-----";\n',
     'static const unsigned char client_PEM_END[26] = "-----END CERTIFICATE-----";'
     ' /* qnx: renamed */\n',
     "After v1.4.10, src/client/ua_client_connect.c got a static array with the "
     "name that plugins/crypto/openssl/ua_pki_openssl.c also uses. Upstream compiles "
     "them as two files; in the single file with OpenSSL they collide (the release is "
     "configured for Mbed TLS, which hides it). The array of the client is renamed."),
    ("open62541.c",
     "UA_BstrstrClient(rc.data, rc.length, openssl_PEM_PRE, 25);\n",
     "UA_BstrstrClient(rc.data, rc.length, client_PEM_END, 25); /* qnx: renamed */\n",
     "The one use of the renamed array."),
]

# Amalgamated sections (one per original source file) that are removed from
# open62541.c. Upstream's CMake compiles this file only with PubSub
# encryption; the single-file release has no preprocessor guard around it.
DROP_SECTIONS = ("/src/pubsub/ua_pubsub_security.c",)

MARK = '/**** amalgamated original file "'


def die(msg):
    sys.exit("port.py: " + msg)


def replace_once(text, old, new, what):
    n = text.count(old)
    if n != 1:
        die(f"{what}: expected the text exactly once, found it {n} times:\n{old[:200]}")
    return text.replace(old, new)


def drop_sections(text):
    parts = text.split(MARK)
    keep, dropped = [parts[0]], []
    for p in parts[1:]:
        name = p.split('"', 1)[0]
        if name in DROP_SECTIONS:
            dropped.append(name)
        else:
            keep.append(p)
    if sorted(dropped) != sorted(DROP_SECTIONS):
        die(f"section drop: expected {list(DROP_SECTIONS)}, found {dropped}")
    return MARK.join(keep)


# ---------------------------------------------------------------------- changes
class Change:
    """One file of patches/: a header of '# key: value' lines and free text,
    then hunks of the form

        @@ hunk [open62541.c|open62541.h]
        <<<<<<< (text of the release)
        =======
        (text of the port)
        >>>>>>>

    A hunk applies to the file named in its first line, or else to the file
    of the header line '# file:'.
    """

    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self.meta = {}
        self.text = []   # the explanation
        self.hunks = []  # (file or None, old, new)
        with open(path, encoding="utf-8", newline="") as fh:
            lines = fh.read().replace("\r\n", "\n").split("\n")
        i = 0
        while i < len(lines) and not lines[i].startswith("@@"):
            line = lines[i]
            if line.startswith("#"):
                body = line[1:].strip()
                key, sep, value = body.partition(":")
                if sep and key in ("title", "file", "upstream", "test") and value.strip():
                    self.meta[key] = value.strip()
                elif body.startswith("open62541-qnx change:"):
                    self.meta["id"] = body.split(":", 1)[1].strip()
                elif not body.startswith("SPDX-License-Identifier"):
                    self.text.append(body)
            elif line.strip():
                die(f"{self.name}: unexpected line before the first hunk: {line!r}")
            i += 1
        while i < len(lines):
            if not lines[i].startswith("@@"):
                if lines[i].strip():
                    die(f"{self.name}: unexpected line between hunks: {lines[i]!r}")
                i += 1
                continue
            words = lines[i].split()
            if words[:2] != ["@@", "hunk"] or len(words) > 3:
                die(f"{self.name}: expected '@@ hunk [file]', found {lines[i]!r}")
            hunk_file = words[2] if len(words) == 3 else None
            if hunk_file is not None and hunk_file not in FILES:
                die(f"{self.name}: unknown file {hunk_file} in {lines[i]!r}")
            i += 1
            if i >= len(lines) or not lines[i].startswith("<<<<<<<"):
                die(f"{self.name}: a hunk must start with '<<<<<<<'")
            i += 1
            old, new = [], []
            while i < len(lines) and lines[i] != "=======":
                old.append(lines[i])
                i += 1
            i += 1
            while i < len(lines) and not lines[i].startswith(">>>>>>>"):
                new.append(lines[i])
                i += 1
            if i >= len(lines):
                die(f"{self.name}: unterminated hunk")
            i += 1
            if not old:
                die(f"{self.name}: a hunk without text of the release")
            self.hunks.append((hunk_file, "\n".join(old) + "\n", "\n".join(new) + "\n"))
        for key in ("id", "title", "file", "upstream", "test"):
            if key not in self.meta:
                die(f"{self.name}: header line '# {key}: ...' is missing")
        if self.meta["file"] not in FILES:
            die(f"{self.name}: unknown file {self.meta['file']}")
        if not self.hunks:
            die(f"{self.name}: no hunk")
        self.hunks = [(f or self.meta["file"], old, new) for f, old, new in self.hunks]
        while self.text and not self.text[0]:
            self.text.pop(0)
        while self.text and not self.text[-1]:
            self.text.pop()


def load_series():
    names = []
    with open(os.path.join(PATCHES, "series"), encoding="utf-8") as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if line:
                names.append(line)
    return [Change(os.path.join(PATCHES, n)) for n in names]


# ------------------------------------------------------------------------ steps
def sha256_of(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def fetch(outdir):
    os.makedirs(outdir, exist_ok=True)
    for name, sha in FILES.items():
        path = os.path.join(outdir, name + ".orig")
        # A file of another release (from before VERSION changed) is replaced.
        if not os.path.exists(path) or sha256_of(path) != sha:
            print("download", BASE + name)
            urllib.request.urlretrieve(BASE + name, path + ".part")
            os.replace(path + ".part", path)
        got = sha256_of(path)
        if got != sha:
            die(f"SHA-256 mismatch for {name}: {got}")
        print("ok      ", name, "sha256", sha[:16] + "...")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = text.encode("utf-8")
    # Leave an identical file alone, so that a build system does not see a change.
    if os.path.exists(path):
        with open(path, "rb") as fh:
            if fh.read() == data:
                return hashlib.sha256(data).hexdigest()
    with open(path, "wb") as fh:
        fh.write(data)
    return hashlib.sha256(data).hexdigest()


def port(outdir):
    text = {}
    for name in FILES:
        with open(os.path.join(outdir, name + ".orig"), encoding="utf-8", newline="") as fh:
            text[name] = fh.read()

    for f, old, new, _why in CONFIG:
        text[f] = replace_once(text[f], old, new, f"configuration of {f}")
    text["open62541.c"] = drop_sections(text["open62541.c"])
    print(f"config   {len(CONFIG)} options, {len(DROP_SECTIONS)} source section dropped")
    for name in FILES:
        write(os.path.join(outdir, "upstream", name), text[name])

    changes = load_series()
    for c in changes:
        for k, (f, old, new) in enumerate(c.hunks, 1):
            text[f] = replace_once(text[f], old, new, f"{c.name} hunk {k}")
        print(f"change   {c.meta['id']:22} {c.meta['title']}")
    manifest = [f"open62541 {VERSION}, ported to QNX 8.0 by open62541-qnx",
                "changes: " + " ".join(c.meta["id"] for c in changes)]
    for name in FILES:
        manifest.append(f"{write(os.path.join(outdir, name), text[name])}  {name}")
    write(os.path.join(outdir, "PORT_MANIFEST.txt"), "\n".join(manifest) + "\n")
    print(f"ported   open62541 {VERSION}: {len(changes)} changes -> {os.path.relpath(outdir)}")


def list_changes():
    for c in load_series():
        print(f"{c.meta['id']}\n    {c.meta['title']}\n    upstream file: {c.meta['upstream']}"
              f"\n    shown by:      {c.meta['test']}\n")


if __name__ == "__main__":
    if "--list" in sys.argv[1:]:
        list_changes()
        sys.exit(0)
    out = os.path.join(ROOT, "third_party")
    fetch(out)
    port(out)
