#!/usr/bin/env python3
"""Fetch the official open62541 single-file release and apply the QNX 8.0 port.

    python scripts/port.py            -> third_party/open62541.{c,h} (patched)

Nothing from upstream is committed to this repository. The release files are
downloaded, checked against pinned SHA-256 hashes, and then every port change
below is applied as an exact text substitution. Each substitution must match
the expected number of times, otherwise the script stops, so a version bump
cannot silently produce a half-ported build.

Standard library only. SPDX-License-Identifier: Apache-2.0
"""
import hashlib
import os
import sys
import urllib.request

VERSION = "v1.4.10"
BASE = f"https://github.com/open62541/open62541/releases/download/{VERSION}/"
FILES = {
    "open62541.c": "b7aa0a50033047d9e9c8c3f246dd8039524e55e3dd021550a27cab238427fa74",
    "open62541.h": "cd0d746e7d5a9cce232e52252366a930b0d2f235634b8040436e180d9089b47a",
}

# (file, old, new, expected count, why)
PATCHES = [
    ("open62541.h", "#define UA_ENABLE_ENCRYPTION_MBEDTLS\n",
     "/* #undef UA_ENABLE_ENCRYPTION_MBEDTLS */ /* qnx: SDP 8.0 has no Mbed TLS */\n", 1,
     "The release is configured for Mbed TLS, which QNX SDP 8.0 does not ship."),
    ("open62541.h", "/* #undef UA_ENABLE_ENCRYPTION_OPENSSL */\n",
     "#define UA_ENABLE_ENCRYPTION_OPENSSL /* qnx: use the OpenSSL 3 that is part of SDP 8.0 */\n", 1,
     "QNX SDP 8.0 ships OpenSSL 3 (libssl.so.3 / libcrypto.so.3) for every target CPU, and "
     "the release already contains the OpenSSL backend, so encryption needs no extra port."),
    ("open62541.h", "#define UA_ENABLE_PUBSUB_ENCRYPTION\n",
     "/* #undef UA_ENABLE_PUBSUB_ENCRYPTION */ /* qnx: upstream implements this on Mbed TLS only */\n", 1,
     "PubSub message encryption (AES-CTR policies) exists upstream only for Mbed TLS."),
]


# Amalgamated source sections (one per original file) removed from open62541.c.
# Upstream's CMake build compiles this file only when PubSub encryption is on;
# the single-file release has no preprocessor guard around it, so with PubSub
# encryption off the section itself must go.
DROP_SECTIONS = ("/src/pubsub/ua_pubsub_security.c",)
DROP_EXPECTED = 1

MARK = '/**** amalgamated original file "'


def drop_sections(text):
    parts = text.split(MARK)
    keep, dropped = [parts[0]], []
    for p in parts[1:]:
        name = p.split('"', 1)[0]
        if name in DROP_SECTIONS:
            dropped.append(name)
        else:
            keep.append(p)
    if len(dropped) != DROP_EXPECTED:
        sys.exit(f"section drop mismatch: expected {DROP_EXPECTED}, got {len(dropped)}: {dropped}")
    print(f"dropped {len(dropped)} amalgamated sections: {', '.join(d.rsplit('/', 1)[1] for d in dropped)}")
    return MARK.join(keep)


def fetch(outdir):
    os.makedirs(outdir, exist_ok=True)
    for name, sha in FILES.items():
        path = os.path.join(outdir, name + ".orig")
        if not os.path.exists(path):
            print("download", BASE + name)
            urllib.request.urlretrieve(BASE + name, path)
        got = hashlib.sha256(open(path, "rb").read()).hexdigest()
        if got != sha:
            sys.exit(f"SHA-256 mismatch for {name}: {got}")
        print("ok", name, sha[:16])


def port(outdir):
    text = {n: open(os.path.join(outdir, n + ".orig"), encoding="utf-8", newline="").read()
            for n in FILES}
    for f, old, new, count, why in PATCHES:
        n = text[f].count(old)
        if n != count:
            sys.exit(f"patch mismatch in {f}: expected {count} x {old!r}, found {n}")
        text[f] = text[f].replace(old, new)
        print(f"patched {f}: {old.strip()[:60]!r}  ({why[:50]}...)")
    text["open62541.c"] = drop_sections(text["open62541.c"])
    for n, t in text.items():
        with open(os.path.join(outdir, n), "w", encoding="utf-8", newline="") as fh:
            fh.write(t)
    print(f"ported open62541 {VERSION}: {len(PATCHES)} changes")


if __name__ == "__main__":
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "third_party")
    fetch(out)
    port(out)
