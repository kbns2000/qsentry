# open62541-qnx: OPC UA on QNX 8.0, with the evidence

[open62541](https://www.open62541.org/) is the open-source C implementation of
OPC UA (IEC 62541), the protocol that PLCs, robots, drives and SCADA/MES
systems use to exchange data. QNX runs many of the controllers underneath
them. Upstream open62541 knows QNX only through a toolchain file for QNX 6.5.

This repository ports the **open62541 v1.4.10** release to **QNX 8.0**, for
**x86_64 and aarch64le**, with **encryption** (OpenSSL 3 of the QNX SDP) and
**PubSub**, and shows on QNX itself why each change is needed:

- **Ten small changes** to the release, each one file with the reason, the
  upstream source file it belongs to, and the test that shows it
  ([`patches/`](patches/)).
- **A test program that runs on QNX** twice: built from the port, and built
  from the unchanged release with the same configuration. The report puts the
  two side by side for every change.
- **A probe of the operating system** (no open62541 in it) that measures the
  QNX behaviours the changes depend on, next to Linux reference values.
- **A demo server**: the live QNX system (CPU load, memory, processes, uptime)
  as OPC UA nodes, encrypted, with user login, a certificate trust folder that
  is re-read while it runs, and log messages in **slog2**.
- **One command** builds both CPUs, makes QEMU images with `mkqnximage`, boots
  them, lets an independent OPC UA client (python asyncua) talk to the demo
  server, and writes the report.

<!-- FILL: results table from the QEMU run -->

## The changes

| # | Change | Why (short) | Shown by |
|---|---|---|---|
| 01 | [Wait in `ppoll()` instead of `select()`](patches/01-eventloop-ppoll.change) | Outside Linux the event loop uses `select()` and never compares a descriptor with `FD_SETSIZE`: a descriptor ≥ `FD_SETSIZE` makes `FD_SET()` write past the `fd_set` on the stack. On QNX 8.0 `FD_SETSIZE` is 256. | `highfd.*`, `robust.idle_client` |
| 02 | [Keep the listen socket when `accept()` finds an aborted connection](patches/02-tcp-accept-aborted.change) | Any `accept()` error but `EINTR` closes the listen socket: the server stops accepting connections. A BSD stack reports a connection reset in the listen queue as `ECONNABORTED`. | `robust.aborted_connect` |
| 03 | [Take `recv() == 0` as the end of the connection](patches/03-tcp-recv-eof.change) | The code also looks at `errno` when `recv()` returns 0; a stale `EAGAIN` keeps a closed connection registered and the event loop spinning. | `robust.peer_close` |
| 04 | [Do not close a UDP socket because of an empty datagram](patches/04-udp-empty-datagram.change) | `recvfrom() == 0` is a valid empty datagram, not an error; one such datagram leaves a PubSub subscriber without a socket. | `pubsub.unicast.empty_datagram` |
| 05 | [Fall back to a socket pair when `pipe()` is not available](patches/05-interrupt-selfpipe.change) | On QNX `pipe()` needs the pipe manager process; images without it cannot start the server. | `nopipe.*` |
| 06 | [Pass the real address length to `getnameinfo()`](patches/06-tcp-peer-name.change) | The size of `sockaddr_storage` is passed instead of the length from `accept()`. | `core.connect.ipv4` |
| 07 | [No reverse DNS query for the wildcard address at start](patches/07-listen-numeric-name.change) | Each listen socket triggers a DNS lookup for a log line; without a reachable DNS server the start waits for the resolver timeout. | `core.startup` |
| 08 | [Ask the QNX headers for the full API](patches/08-qnx-source.change) | `_XOPEN_SOURCE 600` hides QNX functions that the event loop calls. | build log |
| 09 | [Know AArch64 as an IEEE 754 CPU](patches/09-aarch64-ieee754.change) | Without `__STDC_IEC_559__` (glibc defines it, QNX libc does not) Float and Double go through a slow portable routine that changes subnormal values. | `core.float_wire` |
| 10 | [Certificate folders on QNX](patches/10-pki-folders.change) | The trust list folders are "Linux only" in the release although they need only `scandir()`; the change also frees the directory lists, which leaked on every verification. | `security.certfolder.*` |

The build configuration also changes three options of the release header:
OpenSSL instead of Mbed TLS (QNX SDP 8.0 ships OpenSSL 3), and no PubSub
message encryption (which needs Mbed TLS in this version).
[`patches/README.md`](patches/README.md) explains the format of a change.

<!-- FILL: per-change result (release vs port, x86_64 and aarch64le) -->

## Build and run

### What you need

- QNX SDP 8.0 (the free non-commercial licence is enough), on Windows or Linux
- Python 3.9 or newer; CMake 3.16+ and Ninja (`pip install cmake ninja` works)
- QEMU (`qemu-system-x86_64`, `qemu-system-aarch64`) for the QEMU run
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

`all` runs the four steps below. The QEMU run emulates the CPUs in software
and takes a while (both guests run at the same time).

| Step | Command | What it does |
|---|---|---|
| build | `o6q.py build` | `scripts/port.py` downloads the release (SHA-256 checked) and applies [`patches/`](patches/); CMake and Ninja cross-compile for x86_64 and aarch64le with the toolchain file [`cmake/qnx800.toolchain.cmake`](cmake/qnx800.toolchain.cmake). Compiler output: `results/build_<cpu>.log`. |
| image | `o6q.py image --interop` | One QEMU image per CPU with `mkqnximage`: the programs in `/data/o6q`, the SDP's `libcrypto.so.3`, and [`vm/run.sh`](vm/run.sh), which runs at boot. |
| run | `o6q.py run --interop` | Boots both images headless, keeps each console in `results/runs/<time>/<cpu>/serial.log`, starts the host client when the demo server is up, waits for the guests to shut down. |
| report | `o6q.py report` | Writes `SUMMARY.md` next to the logs: suites per CPU, every change with its tests for the port and the release, latencies, host client, demo server, and the operating system probe next to Linux. |

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
  accepted. The folder is read again for every connection (change 10), so a
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

## Repository layout

| Path | Content |
|---|---|
| [`patches/`](patches/) | the ten changes (MPL-2.0), `series`, format description |
| [`scripts/port.py`](scripts/port.py) | download, verify, configure and patch the release |
| [`scripts/o6q.py`](scripts/o6q.py) | build, image, run, report |
| [`scripts/o6q_client.py`](scripts/o6q_client.py) | host client (python asyncua) for the demo server |
| [`src/`](src/) | `o6q_check` and its suites, `o6q_envprobe`, `o6q_server` |
| [`src/qnx/`](src/qnx/) | QNX system information (system page, procfs) and the slog2 logger |
| [`vm/`](vm/) | what runs on the QNX guest |
| [`cmake/`](cmake/) | CMake toolchain file for QNX SDP 8.0 |
| [`tools/`](tools/) | compile-time API probe of the SDP, a mock server to test the host client |
| [`results/`](results/) | logs and reports of the runs |

## How this was made

This port was written by an AI coding agent (Claude) working with one person
who ran the QNX SDP and QEMU on their PC. The agent read the open62541 event
loop for assumptions that hold on Linux but not necessarily on QNX, wrote the
operating system probe to measure them, and kept a change only where the
measurement on QNX showed the problem. Every number in this repository comes
from a log under [`results/`](results/).

## Licence

The changes in [`patches/`](patches/) modify open62541 and are licensed like
it under the Mozilla Public License 2.0 ([`patches/LICENSE-MPL-2.0`](patches/LICENSE-MPL-2.0)).
Everything else in this repository is licensed under the Apache License 2.0
([`LICENSE`](LICENSE)). open62541 itself is downloaded at build time and not
part of this repository.
