# Devpost submission text (open62541-qnx)

**Project name:** open62541-qnx

**Tagline:** OPC UA for QNX 8.0: open62541 with encryption and PubSub on x86_64
and aarch64le, and every change to it shown on QNX itself.

**Category:** Open-source port

**Video:** (YouTube link)

**Repository:** https://github.com/kbns2000/open62541-qnx

## Inspiration

OPC UA (IEC 62541) is how PLCs, robots, drives and SCADA/MES systems exchange
data on the factory floor, and QNX runs many of the controllers underneath
them. open62541 is the open-source C implementation of OPC UA, but upstream it
knows QNX only through a toolchain file for QNX 6.5.

Getting it to *compile* for QNX 8.0 is quick. The harder question is whether
code that is tested on Linux *behaves* the same way on QNX. A port that only
compiles moves that question to the first customer. We wanted a port that
answers it with measurements on QNX.

## What it does

- Ports the open62541 **v1.4.10** release to **QNX 8.0** for **x86_64 and
  aarch64le**, with **encryption** (OpenSSL 3 from the QNX SDP: Basic256Sha256,
  Aes128_Sha256_RsaOaep, Aes256_Sha256_RsaPss) and **PubSub**.
- The port is **ten small changes** to the unchanged release, each a file with
  the reason, the upstream source file it belongs to, and the test that shows
  it. A script downloads the release, checks its SHA-256 and applies them.
- **A test program runs on QNX twice**: built from the port, and built from the
  unchanged release with the same configuration. The report shows, for every
  change, the release failing and the port passing on the same QNX guest.
- **An operating system probe** (no open62541 in it) measures the behaviours
  the changes depend on and prints them next to Linux reference values.
- **A demo server** publishes the live QNX system (CPU load per CPU, memory,
  processes, threads, uptime, `ClockCycles()` frequency) as OPC UA nodes,
  encrypted, with user login, a certificate trust folder that is re-read for
  every connection, and the stack's log messages in **slog2**.
- **One command** builds both CPUs, makes QEMU images with `mkqnximage`, boots
  them, lets an independent OPC UA client (python asyncua) talk to the demo
  server through QEMU's port forwarding, and writes the report.

<!-- FILL: results -->

## How we built it

- open62541's single-file release, unchanged in the repository; the changes are
  applied by `scripts/port.py`, which fails if any replaced text is not found
  exactly once.
- C99 on QNX 8.0 APIs for the demo server: system page (`_syspage_ptr`),
  `ClockCycles()`, `/proc` for processes and threads, slog2 for logging.
- CMake with a QNX SDP 8.0 toolchain file, `qcc` for both CPUs, `mkqnximage`
  for the images, QEMU (TCG) on a Windows PC.
- Python (standard library) for the automation and the report; asyncua for the
  independent client.
- Built by an AI coding agent (Claude) with one person, who ran the SDP and QEMU
  on their PC. The agent read the open62541 event loop for assumptions that hold
  on Linux, wrote the probe to measure them on QNX, and kept a change only where
  the measurement showed the problem.

## Challenges we ran into

<!-- FILL -->

## Accomplishments that we're proud of

<!-- FILL -->

## What we learned

<!-- FILL -->

## What's next

- Offer the changes upstream to open62541, one pull request per change, with
  the tests.
- Run the suites on real aarch64 hardware (Raspberry Pi 4 with QNX 8.0).
- PubSub message encryption needs Mbed TLS in open62541 v1.4; an OpenSSL
  backend for the AES-CTR PubSub policies would remove that gap.

## Built with

C, open62541, QNX SDP 8.0, qcc, OpenSSL 3, slog2, CMake, Ninja, mkqnximage,
QEMU, Python, asyncua
