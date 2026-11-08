# open62541-qnx: OPC UA on QNX 8.0

This repository ports [open62541](https://www.open62541.org/), the most widely
used open-source OPC UA stack (MPL-2.0, C99), to **QNX 8.0**. It includes an
end-to-end check that runs on the target.

OPC UA is how PLCs, robots, drives and MES/SCADA systems exchange data on the
factory floor. QNX runs many of the controllers underneath them. Upstream
open62541 ships only a QNX **6.5** toolchain file (`tools/cmake/Toolchain-QNX-6.5.0.cmake`),
and the QNX open-source ports collection (`qnx-ports/build-files`) does not
include it.

This is a new repository, not a fork. Nothing from upstream is committed.
`scripts/port.py` downloads the official v1.4.10 single-file release, checks
SHA-256, and applies the port.

## Results

_Filled in from the QEMU run. See [`results/`](results/)._

## What the port changes, and why

| Change | Why |
|---|---|
| Disable `UA_ENABLE_ENCRYPTION`, `UA_ENABLE_ENCRYPTION_MBEDTLS` and `UA_ENABLE_PUBSUB_ENCRYPTION` | The release is configured for Mbed TLS, which QNX 8.0 does not ship. This base port uses security policy *None*. |
| Disable `UA_ENABLE_PUBSUB` and `UA_ENABLE_PUBSUB_INFORMATIONMODEL`, and **drop the 19 amalgamated `src/pubsub/*` sections** from `open62541.c` | The single-file release compiles PubSub unconditionally, and that code assumes PubSub encryption. Turning the feature off in the header leaves undefined types. The sections themselves must go. |
| Link with `-lsocket` | On QNX the BSD socket API lives in libsocket. |
| Nothing else | The POSIX event loop already avoids Linux-only paths (`epoll`, `signalfd` and `AF_PACKET` are behind `__linux__`). It falls back to `select` and a self-pipe, and that fallback works on QNX 8.0. |

Every substitution in `scripts/port.py` must match an exact expected count, and
the section drop must remove exactly 19 sections. A different upstream version
therefore fails loudly instead of producing a half-ported build.

## The end-to-end check

`src/opcua_qnx_check.c` starts an OPC UA server on a thread, connects a client
over TCP loopback, and checks the services real clients use:

| test | what passes |
|---|---|
| startup | the server binds `opc.tcp://localhost:4840` |
| connect | a session is created |
| browse | the Objects folder contains our `answer` and `add` nodes and the standard `Server` object |
| read | 1000 reads of an Int32 all return 42, with latency recorded |
| write | 1000 writes succeed, and the read-back matches the last value |
| call | 100 calls of method `add(a,b)` return `a+b` |
| subscribe | 10 ms sampling of a counter updated by a 10 ms server timer: at least 20 notifications in 2 s, none out of order |
| qnxinfo | a data-source node computed from QNX system state (`_syspage_ptr->num_cpu`, `ClockCycles()`) is readable over OPC UA |

It prints `O6Q PASS|FAIL ...` lines, latency lines and
`O6Q SUMMARY pass=<p> fail=<f>`, and exits with the number of failures.

## Build and run (Windows host, no hardware)

You need QNX SDP 8.0 (`%USERPROFILE%\qnx800`), QEMU for Windows and Python 3.

```bat
git clone https://github.com/kbns2000/open62541-qnx.git
cd open62541-qnx
build.cmd       :: fetch + port + qcc (a few minutes: open62541.c is 13 MB)
run_qemu.cmd    :: mkqnximage + QEMU boot + run the check + keep the console log
```

On a Linux or macOS host with SDP 8.0:

```sh
python3 scripts/port.py
qcc -Vgcc_ntox86_64 -O1 -w -c -Ithird_party -o open62541.o third_party/open62541.c
qcc -Vgcc_ntox86_64 -O2 -Wall -Ithird_party -o opcua_qnx_check src/opcua_qnx_check.c open62541.o -lsocket -lm
```

For ARM targets use `-Vgcc_ntoaarch64le` instead (untested).

## What's next

- Port Mbed TLS so the *Basic256Sha256* and *Aes128Sha256RsaOaep* security
  policies can be enabled. Those are what production deployments require.
- Restore PubSub (UDP multicast) once encryption is available.

## Licence

The port scripts and check are Apache-2.0. open62541 is MPL-2.0 and is
downloaded at build time, not redistributed here.
