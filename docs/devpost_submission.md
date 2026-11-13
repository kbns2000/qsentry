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

**Results** (one run on a QEMU x86_64 guest, QNX 8.0.0):

| suite | port: pass / fail | unchanged v1.4.22: pass / fail |
|---|---|---|
| core | 29 / 1 | 29 / 1 |
| security | 23 / 0 | 15 / 5 |
| pubsub | 14 / 0 | 7 / 3 |
| highfd | 6 / 0 | 4 / 2 |
| robust | 5 / 1 | 2 / 4 |
| nopipe | 3 / 0 | 0 / 2 |

- All 17 tests named by the seven changes pass with the port. With the
  unchanged release 15 fail, and 2 cannot run because the step before them
  failed.
- python asyncua on the Windows host against the demo server on QNX: 8 of 8
  checks pass. That covers three encrypted security policies, user login,
  17 live QNX values, a subscription with 15 changes in 15 s, and the refusal
  of a wrong password and of an untrusted certificate.
- The two failures of the port fail with the release as well, and are listed
  in the report.

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

- Each QEMU run found something that the next one fixed:
  - **The demo server refused to start with BadTypeMismatch.** v1.4.22 wants
    ArrayDimensions for every one-dimensional variable and says so only at
    log level INFO.
  - **Multicast PubSub failed in both builds** with "Can't assign requested
    address". open62541 passes a whole `struct ip_mreq` to `IP_MULTICAST_IF`.
    QNX's BSD stack reads the first four bytes, the multicast group, as the
    interface address. This became change 07.
  - **The guest sent packets but received none** (`netstat -in`: Ipkts 0).
    The image's `pci_hw.cfg` has interrupt routing for one PCI slot only. The
    NIC now sits in that slot (0x17), and the host client got through.
- One planned change was dropped after a measurement. QNX's `getnameinfo()`
  accepts the size of `sockaddr_storage`, so passing the real address length
  fixes nothing there.

## Accomplishments that we're proud of

- Seven changes, each with a test that **fails on QNX with the unchanged
  release and passes with the port**. The tests run on the same QNX guest,
  in the same run.
- Two of the faults are not QNX-specific and affect open62541 on other
  systems too:
  - the `PATH_MAX` stack overflow in the certificate folder code;
  - multicast PubSub on BSD-derived network stacks. That is tested here on
    QNX only; for FreeBSD it follows from its source.
- An independent OPC UA stack (python asyncua) reads live QNX system data
  over an encrypted, authenticated session.
- Every number in the README and here is a line of a console log in the
  repository.

## What we learned

- "Compiles for QNX" and "works on QNX" are far apart. Five of the seven
  faults are invisible on Linux, because Linux uses epoll, has a larger
  `FD_SETSIZE`, reports a reset differently, and accepts either form of the
  multicast option.
- Measure first, change second: a small probe without open62541 decided
  which suspected problems were real. One of them was not, and was dropped.
- QNX's tools are good at telling you what happened: `slog2info`, `pidin`,
  and `netstat -in` with packet counters found the network fault in one run.

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
