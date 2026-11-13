# Devpost submission text (open62541-qnx)

**Project name:** open62541-qnx

**Tagline:** OPC UA on QNX 8.0: the newest open62541 1.4 release, tested on QNX
against itself, with a small port for what QNX shows to be broken.

**Category:** Open-source port

**Video:** (YouTube link)

**Repository:** https://github.com/kbns2000/open62541-qnx

## Inspiration

OPC UA (IEC 62541) is how PLCs, robots, drives and SCADA/MES systems exchange
data on the factory floor, and QNX runs many of the controllers underneath
them. open62541 is the open-source C implementation of OPC UA. It builds for
QNX since v1.4.12, but its platform table lists QNX as tier 3: no continuous
integration and no test system. Code that is tested on Linux every day runs on
QNX untested.

Getting it to *compile* for QNX 8.0 is quick. The harder question is whether
it *behaves* the same way on QNX, whose network stack comes from FreeBSD and
whose pipes are served by a separate process. A port that only compiles moves
that question to the first customer. We wanted answers measured on QNX.

## What it does

- Takes the newest release of the 1.4 line, **open62541 v1.4.22**, for
  **QNX 8.0** with **encryption** (OpenSSL 3 from the QNX SDP) and **PubSub**,
  cross-compiled for **x86_64 and aarch64le**.
- **One test program runs on QNX twice**: built from the unchanged release and
  built from the port. The report shows, for every change, what the release
  does on the same QNX guest and what the port does.
- **An operating system probe** (no open62541 in it) measures the behaviours
  the changes depend on and prints them next to Linux reference values.
- **The port is seven small changes**, each a file with the reason, the
  upstream source file it belongs to, and the tests that show it. A script
  downloads the release, checks its SHA-256 and applies them. Changes that the
  measurements could not show were removed.
- **A demo server** publishes the live QNX system (CPU load per CPU, memory,
  processes, threads, uptime, `ClockCycles()` frequency) as OPC UA nodes,
  encrypted, with user login, a certificate trust folder that is re-read for
  every connection, and the stack's log messages in **slog2**.
- **One command** builds, makes a QEMU image with `mkqnximage`, boots it, lets
  an independent OPC UA client (python asyncua) talk to the demo server
  through QEMU's port forwarding, and writes the report.

<!-- FILL: results -->

## How we built it

- open62541's single-file release, never committed; `scripts/port.py`
  downloads it, checks the hash, sets the build configuration and applies the
  changes, and fails if any replaced text is not found exactly once.
- C99 on QNX 8.0 APIs for the demo server: system page (`_syspage_ptr`),
  `ClockCycles()`, `/proc` for processes and threads, slog2 for logging.
- CMake with a QNX SDP 8.0 toolchain file, `qcc` for both CPUs, `mkqnximage`
  for the image, QEMU (TCG) on a Windows PC.
- Python (standard library) for the automation and the report; asyncua for the
  independent client.
- Built by an AI coding agent (Claude) with one person, who ran the SDP and
  QEMU on their PC. The agent read the open62541 event loop for assumptions
  that hold on Linux, wrote the probe to measure them on QNX, and kept a change
  only where the measurement showed the problem.

## Challenges we ran into

- The single-file release of v1.4.22 does not compile with OpenSSL: two of its
  source files define the same static array, which only collide when they are
  one file (the release is configured for Mbed TLS, which QNX SDP 8.0 does not
  ship). `port.py` renames one of them.
- QNX SDP 8.0 as installed here has no QEMU board support for aarch64le
  (`startup-qemu-virt`), so aarch64le is cross-compiled but not run.
- The agent could not run QNX itself. Every question had to be answered by one
  unattended run on the PC, so the probe and the tests record everything a
  later reader needs, and the report is generated from the console log alone.

<!-- FILL: challenges from the run -->

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

## Related work

Upstream open62541 defines `_QNX_SOURCE` on QNX since v1.4.12, and
conan-center-index has a recipe that builds it for QNX. Two other hackathon
projects port open62541 with general porting tools: PortPilot (v1.5.10,
upstream's unit tests on a QNX 8.0 QEMU target) and qnx-porter (v1.4.6, built
as a static library). This project writes tests for the places where QNX
differs from Linux and runs them against the unchanged release as well.

## Built with

C, open62541, QNX SDP 8.0, qcc, OpenSSL 3, slog2, CMake, Ninja, mkqnximage,
QEMU, Python, asyncua
