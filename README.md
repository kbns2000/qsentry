# open62541-qnx: OPC UA on QNX 8.0, with the evidence

[open62541](https://www.open62541.org/) is the open-source C implementation of
OPC UA (IEC 62541), the protocol that PLCs, robots, drives and SCADA/MES
systems use to exchange data. QNX runs many of the controllers underneath
them.

open62541 builds for QNX since v1.4.12, and its
[platform table](https://github.com/open62541/open62541/blob/master/arch/README.md)
lists QNX as tier 3: no continuous integration, no test system. So the code
compiles for QNX 8.0, but nobody checks whether it *behaves* there as on
Linux, where it is tested. This repository checks that for the newest release
of the 1.4 line, **open62541 v1.4.22**, on **QNX 8.0**, with **encryption**
(OpenSSL 3 of the QNX SDP) and **PubSub**:

- **A test program that runs on QNX** twice: built from the unchanged release,
  and built from the port. The report puts the two side by side for every
  change.
- **Six small changes** to the release, each one file with the reason, the
  upstream source file it belongs to, and the test that shows it
  ([`patches/`](patches/)). A change stays only where the test on QNX shows
  the difference.
- **A probe of the operating system** (no open62541 in it) that measures the
  QNX behaviours the changes depend on, next to Linux reference values.
- **A demo server**: the live QNX system (CPU load, memory, processes, uptime)
  as OPC UA nodes, encrypted, with user login, a certificate trust folder that
  is re-read for every connection, and log messages in **slog2**.
- **One command** cross-compiles for x86_64 and aarch64le, makes a QEMU image
  with `mkqnximage`, boots it, lets an independent OPC UA client (python
  asyncua) talk to the demo server, and writes the report.

<!-- FILL: results table from the QEMU run -->

## The changes

| # | Change | Why (short) | Shown by |
|---|---|---|---|
| 01 | [Wait in `ppoll()` instead of `select()`](patches/01-eventloop-ppoll.change) | Outside Linux the event loop uses `select()` and never compares a descriptor with `FD_SETSIZE`: a descriptor ≥ `FD_SETSIZE` makes `FD_SET()` write past the `fd_set` on the stack. | `highfd.client`, `highfd.sessions`, `robust.idle_client` |
| 02 | [Keep the listen socket when `accept()` finds an aborted connection](patches/02-tcp-accept-aborted.change) | Any `accept()` error but `EINTR` closes the listen socket: the server stops accepting connections. A BSD network stack reports a connection reset in the listen queue as `ECONNABORTED`. | `robust.aborted_connect` |
| 03 | [Take `recv() == 0` as the end of the connection](patches/03-tcp-recv-eof.change) | The code also looks at `errno` when `recv()` returns 0; a stale `EAGAIN` keeps a closed connection registered and the event loop spinning. | `robust.peer_close` |
| 04 | [Do not close a UDP socket because of an empty datagram](patches/04-udp-empty-datagram.change) | `recvfrom() == 0` is a valid empty datagram, not an error; one such datagram leaves a PubSub subscriber without a socket. | `pubsub.unicast.empty_datagram` |
| 05 | [Fall back to a socket pair when `pipe()` is not available](patches/05-interrupt-selfpipe.change) | On QNX `pipe()` needs the pipe manager process; images without it cannot start the server. | `nopipe.*` |
| 06 | [Certificate folders on QNX](patches/06-pki-folders.change) | The trust list folders are "Linux only" in the release although they need only `scandir()`. The change also refuses folder names of `PATH_MAX` or more, which overflow a stack buffer in the release (on Linux as well). | `security.certfolder.*` |

[`patches/README.md`](patches/README.md) explains the format of a change.
`scripts/port.py` also sets the build configuration of the release header:
OpenSSL instead of Mbed TLS (QNX SDP 8.0 ships OpenSSL 3), and no PubSub
message encryption (which needs Mbed TLS in this version). With OpenSSL the
single-file release of v1.4.22 does not compile: two of its source files
define a static array `openssl_PEM_PRE`, which collide in the one file, so
`port.py` renames the one of the client. Both builds, release and port, get
this configuration.

<!-- FILL: per-change result (release vs port) -->

## Build and run

### What you need

- QNX SDP 8.0 (the free non-commercial licence is enough), on Windows or Linux
- Python 3.9 or newer; CMake 3.16+ and Ninja (`pip install cmake ninja` works)
- QEMU (`qemu-system-x86_64`) for the QEMU run
- For the host client: `pip install asyncua`

### Everything in one go

```bat
:: Windows, in cmd.exe
call %USERPROFILE%\qnx800\qnxsdp-env.bat
git clone https://github.com/kbns2000/open62541-qnx.git
cd open62541-qnx
python scripts\o6q.py all --interop
```

```sh
# Linux
source ~/qnx800/qnxsdp-env.sh
git clone https://github.com/kbns2000/open62541-qnx.git
cd open62541-qnx
python3 scripts/o6q.py all --interop
```

`all` runs the four steps below. The QEMU run emulates the CPU in software and
takes a while.

| Step | Command | What it does |
|---|---|---|
| build | `o6q.py build` | `scripts/port.py` downloads the release (SHA-256 checked) and applies [`patches/`](patches/); CMake and Ninja cross-compile for x86_64 and aarch64le with the toolchain file [`cmake/qnx800.toolchain.cmake`](cmake/qnx800.toolchain.cmake). Compiler output: `results/build_<cpu>.log`. |
| image | `o6q.py image --interop` | A QEMU image per CPU with `mkqnximage`: the programs in `/data/o6q`, the SDP's `libcrypto.so.3`, and [`vm/run.sh`](vm/run.sh), which runs at boot. |
| run | `o6q.py run --interop` | Boots the images headless, keeps each console in `results/runs/<time>/<cpu>/serial.log`, starts the host client when the demo server is up, waits for the guests to shut down. |
| report | `o6q.py report` | Writes `SUMMARY.md` next to the logs: the suites, every change with its tests for the release and the port, latencies, host client, demo server, and the operating system probe next to Linux. |

aarch64le is cross-compiled but not run: for aarch64le `mkqnximage` needs
`startup-qemu-virt`, which is not part of the SDP 8.0 installation used here
(`Host file 'startup-qemu-virt' not available`, see
[`results/o6q_image.log`](results/o6q_image.log)). With that board support
package installed, `o6q.py` builds and boots the aarch64le image as well.

### Only the library

```sh
python3 scripts/port.py          # third_party/open62541.c and .h, ported
qcc -Vgcc_ntoaarch64le -O2 -c -fno-strict-aliasing -Ithird_party \
    -o open62541.o third_party/open62541.c
qcc -Vgcc_ntoaarch64le -O2 -Ithird_party -o myserver myserver.c open62541.o \
    -lsocket -lcrypto -lm
```

`-Vgcc_ntox86_64` for x86_64. Or use the `open62541` target of the
[`CMakeLists.txt`](CMakeLists.txt) (static library). The QNX target needs
`libcrypto.so.3` of the SDP at run time.

## The demo server

```sh
o6q_server -p 4840 --secure --trust /data/pki/trusted --user operator:secret
```

It publishes the QNX system below `Objects/QNX` (namespace
`urn:open62541-qnx:system`): `OS`, `Release`, `Machine`, `Hostname`,
`CpuCount` (system page), `CyclesPerSecond` (`ClockCycles()`), `ClockPeriodNs`,
`RamTotalMB`, and once per second `Uptime`, `RamFreeMB`, `Processes`,
`Threads`, `CpuLoad` (per CPU), `CpuLoadAverage`, `ProcessList`,
`OpcUaSessions`. Any OPC UA client can browse and subscribe, for example
UaExpert or [`scripts/o6q_client.py`](scripts/o6q_client.py).

- `--secure`: encrypted endpoints with a self-signed RSA-2048 certificate made
  at start.
- `--trust DIR`: only client certificates that are files in `DIR` are
  accepted. The folder is read again for every connection (change 06), so a
  client is admitted or locked out by copying or deleting one file, without a
  restart.
- `--user name:password`: anonymous sessions are refused.
- The stack's log messages go to slog2: `slog2info -b o6q_server`.

## How the evidence is collected

[`vm/run.sh`](vm/run.sh) runs on the QNX guest at boot:

1. `o6q_envprobe`: what the operating system does, without open62541
   (`FD_SETSIZE`, descriptor limit, `accept()` after a reset, `O_NONBLOCK`
   inheritance, `recv()` and `errno`, empty UDP datagrams, `getnameinfo()`,
   `pipe()`, the C library's floating point macros, ...).
2. `o6q_check <suite>` and `o6q_check_upstream <suite>` for the suites
   `core`, `security`, `pubsub`, `highfd`, `robust`: a server and clients in
   one process over loopback, each test one `O6Q PASS|FAIL` line.
3. The pipe manager is stopped and the `nopipe` suite runs for both builds.
4. With `--interop`: the demo server, and the host client connects through
   QEMU's port forwarding.

Every claim in the report is a line of a console log under `results/`.
[`tools/feature_macros.py`](tools/feature_macros.py) shows at compile time
which QNX declarations the feature macros of `open62541.h` make visible.

## Related work

- Upstream open62541 defines `_QNX_SOURCE` on QNX since v1.4.12, so that the
  release compiles there; the changes here are about behaviour, not about the
  build.
- [conan-center-index](https://github.com/conan-io/conan-center-index/tree/master/recipes/open62541)
  has a recipe that builds open62541 for QNX (Neutrino).
- Two other projects of the QNX hackathon port open62541 with general porting
  tools: [PortPilot](https://github.com/HyunsikParker/portpilot) builds
  v1.5.10 and runs upstream's unit tests on a QNX 8.0 x86_64 QEMU target, and
  [qnx-porter](https://github.com/thegoodengineers/qnx-porter) builds v1.4.6
  as a static library. This repository instead writes tests for the places
  where QNX differs from Linux, and runs them against the unchanged release
  as well.

## How this was made

This port was written by an AI coding agent (Claude) working with one person
who ran the QNX SDP and QEMU on their PC. The agent read the open62541 event
loop for assumptions that hold on Linux but not necessarily on QNX, wrote the
operating system probe to measure them, and kept a change only where the
measurement on QNX showed the problem. Every number in this repository comes
from a log under [`results/`](results/).

## Repository layout

| Path | Content |
|---|---|
| [`patches/`](patches/) | the changes (MPL-2.0), `series`, format description |
| [`scripts/port.py`](scripts/port.py) | download, verify, configure and patch the release |
| [`scripts/o6q.py`](scripts/o6q.py) | build, image, run, report |
| [`scripts/o6q_client.py`](scripts/o6q_client.py) | host client (python asyncua) for the demo server |
| [`src/`](src/) | `o6q_check` and its suites, `o6q_envprobe`, `o6q_server` |
| [`src/qnx/`](src/qnx/) | QNX system information (system page, procfs) and the slog2 logger |
| [`vm/`](vm/) | what runs on the QNX guest |
| [`cmake/`](cmake/) | CMake toolchain file for QNX SDP 8.0 |
| [`tools/`](tools/) | compile-time probes of the SDP, a mock server to test the host client, the video renderer |
| [`results/`](results/) | logs and reports of the runs |

## Licence

The changes in [`patches/`](patches/) modify open62541 and are licensed like
it under the Mozilla Public License 2.0 ([`patches/LICENSE-MPL-2.0`](patches/LICENSE-MPL-2.0)).
Everything else in this repository is licensed under the Apache License 2.0
([`LICENSE`](LICENSE)). open62541 itself is downloaded at build time and not
part of this repository.
