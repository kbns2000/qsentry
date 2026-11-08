# Codelab: your first QNX 8.0 resource manager, built on Windows and booted in QEMU

**Time:** about 45 minutes (most of it is the one-time SDP install)
**You will build:** `tickdev`, a ~150-line daemon that
- registers `/dev/tick` in the QNX path space (a *resource manager*),
- receives a periodic timer **pulse** on the same channel, and
- measures its own wake-up jitter with `ClockCycles()`.

You will then put it into a QNX 8.0 x86_64 image with `mkqnximage`, boot that image
in QEMU on a plain Windows PC (no Linux VM, no target board), and talk to the
daemon with `cat` and `echo`.

This is the exact path used to bring up [qsentry](../README.md) for the QNX
Global Hackathon. The first five pitfalls in step 7 are ones we actually hit.

---

## 1. What you need

| Item | Notes |
|---|---|
| Windows 10/11 x64 | Tested on Windows 11 with PowerShell / `cmd` |
| QNX SDP 8.0 | Free non-commercial licence from qnx.com, installed with QNX Software Center. This codelab assumes the default `%USERPROFILE%\qnx800`. |
| QEMU for Windows | The `qemu-w64-setup` installer, default folder `C:\Program Files\qemu` |
| Git | To clone this repository |

No Linux, WSL, Docker or target hardware is needed.

Open a `cmd` window and load the SDP environment once per window:

```bat
call %USERPROFILE%\qnx800\qnxsdp-env.bat
qcc -V
```

`qcc -V` should list `gcc_ntox86_64` among the targets. If it says
`'qcc' is not recognized`, the env script was not called in this window.

## 2. Get the code

```bat
git clone https://github.com/kbns2000/qsentry.git
cd qsentry\codelab
```

```
codelab/
  src/tickdev.c        the daemon (read this alongside step 3)
  build.cmd            qcc cross-compile
  run_qemu.cmd         mkqnximage + QEMU boot + console capture
  vm/check.sh          script that runs inside QNX at boot
  vm/post_start.custom mkqnximage hook that starts check.sh
```

## 3. Walk through the daemon

### 3.1 A resource manager in four calls

A QNX resource manager is an ordinary process that *owns a pathname*. When any
program calls `open("/dev/tick")`, the kernel's path manager routes the request
to us as a message, and our `read`/`write` handlers answer it.

```c
dispatch_t *dpp = dispatch_create();                         // 1. a channel + dispatch layer
iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cfuncs,
                 _RESMGR_IO_NFUNCS, &iofuncs);               // 2. default POSIX behaviour
iofuncs.read  = io_read;                                     //    ...override only what we need
iofuncs.write = io_write;
iofunc_attr_init(&attr, S_IFNAM | 0666, NULL, NULL);
resmgr_attach(dpp, &rattr, "/dev/tick", _FTYPE_ANY, 0,
              &cfuncs, &iofuncs, &attr);                     // 3. claim the path
for (;;) { ctp = dispatch_block(ctp); dispatch_handler(ctp); } // 4. serve messages
```

`iofunc_func_init()` fills every handler with the stock `iofunc_*_default`
implementation, so `open`, `close`, `stat`, permissions and so on work without
any code from us.

### 3.2 The read handler and the file offset

`cat` keeps calling `read()` until it gets 0 bytes. If the handler ignores the
offset, `cat /dev/tick` prints forever. So we keep track of how much of the
line this open file descriptor has already consumed (`ocb->offset`):

```c
off_t  left = (off_t)len - ocb->offset;
size_t n = left > 0 ? min(left, want) : 0;
SETIOV(ctp->iov, line + ocb->offset, n);   // reply straight from our buffer
ocb->offset += n;
_IO_SET_READ_NBYTES(ctp, n);
return _RESMGR_NPARTS(n ? 1 : 0);
```

The reply goes out as an IOV that points at our buffer, so there is no extra copy.

### 3.3 The write handler

The client's data follows the `io_write_t` header in the message. If it is
larger than the receive buffer it may not have arrived yet, so always fetch it with
`resmgr_msgget()` rather than reading past the header:

```c
resmgr_msgget(ctp, buf, n, sizeof(msg->i));
```

`echo reset > /dev/tick` zeroes the counters, and `echo "period 5" > /dev/tick`
re-arms the timer at 5 ms.

### 3.4 Timer → pulse → handler, all on one thread

A *pulse* is a tiny non-blocking message: 8 bits of code and a value. We ask the
dispatch layer for a pulse code of our own, open a connection back to our own
channel, and tell a POSIX timer to deliver that pulse:

```c
int code = pulse_attach(dpp, MSG_FLAG_ALLOC_PULSE, 0, on_tick, NULL);
int coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
struct sigevent ev;
SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, code, 0);
timer_create(CLOCK_MONOTONIC, &ev, &timer);
```

Now the single `dispatch_block()` loop receives both client I/O and timer ticks.
Because there is only one thread, there are no locks: `on_tick()` and
`io_read()` can never run at the same time.

`MSG_FLAG_SIDE_CHANNEL` keeps this internal connection id out of the range used
for file descriptors.

### 3.5 Measuring jitter

```c
uint64_t now = ClockCycles();
double dt_us = (now - last) * 1e6 / SYSPAGE_ENTRY(qtime)->cycles_per_sec;
double jit   = fabs(dt_us - period_ms * 1000.0);
```

`ClockCycles()` reads the free-running cycle counter. The system page tells us
its rate. The first two ticks after a (re)arm are skipped so the start-up
transient does not count.

## 4. Build

```bat
build.cmd
```

which runs

```bat
qcc -Vgcc_ntox86_64 -O2 -Wall -Wextra -o out\tickdev src\tickdev.c
```

Expected: `BUILD OK: out\tickdev`. The output is an x86_64 QNX ELF. It will
not run on Windows.

## 5. Make an image and boot it

```bat
run_qemu.cmd
```

The script does four things:

1. **Payload.** It copies `out\tickdev` and `vm\check.sh` to `%USERPROFILE%\tickdev-vm\payload`.
2. **Snippets.** It writes two `mkqnximage` snippets under `local\snippets\`:
   - `data_files.custom` puts the files into the image's `/data` partition:
     ```
     [uid=0 gid=0 perms=0755] codelab/tickdev=C:/Users/you/tickdev-vm/payload/tickdev
     [uid=0 gid=0 perms=0755] codelab/check.sh=C:/Users/you/tickdev-vm/payload/check.sh
     ```
   - `post_start.custom` runs our script once the system is up:
     ```
     sh /data/codelab/check.sh &
     ```
3. **Image.** It builds the image:
   ```bat
   mkqnximage --noprompt --force --type=qemu --arch=x86_64 --hostname=tickdev --ssh-ident=none --build
   ```
4. **Boot.** It boots the image headless, sending the QNX console to a file:
   ```bat
   qemu-system-x86_64 -accel tcg -cpu max -display none -m 1G -smp 2 ^
     -kernel output\ifs.bin -drive file=output\disk-qemu.vmdk,if=ide,id=drv0 ^
     -nic none -serial file:serial.log
   ```
   `check.sh` ends with `shutdown`, and because QEMU runs with `-no-reboot` that
   ends the VM. The script waits for *its own* QEMU process (`-pidfile qemu.pid`),
   capped at 10 minutes, then prints `serial.log`. A copy is kept in
   `results\serial_tickdev.log`.

## 6. Read the result

Look for the block between the two markers:

```
=== TICKDEV CHECK START
QNX tickdev 8.0.0 ... x86pc x86_64
tickdev: /dev/tick ready, period 10 ms, <N> cycles/s
ticks=<about 100> max_jitter_us=<J> period_ms=10
ticks=<about 300> max_jitter_us=<J> period_ms=10
ticks=<about 200> max_jitter_us=<J> period_ms=5
ticks=0 max_jitter_us=0 period_ms=5
     pid tid name               prio STATE       Blocked
  ...    1 data/codelab/tickdev  10r RECEIVE     1
=== TICKDEV CHECK END
```

Things to check:

- **`ticks`** grows by about 100 per second at 10 ms and about 200 per second at 5 ms.
- **`max_jitter_us`** is the worst wake-up error seen so far.
  Under QEMU **TCG** (software CPU emulation) it is much larger than on real
  hardware or with a hardware accelerator. Treat it as a property of the
  emulator, not of QNX.
- **`pidin`** shows the daemon `RECEIVE`-blocked. It uses no CPU between ticks
  and requests.

The measured log from our own run is in [`results/`](results/).

## 7. Pitfalls we hit, and the fixes

| Symptom | Cause | Fix |
|---|---|---|
| `error: request for member 'offset' in something not a structure` in `io_read` | In QNX 8.0 headers, `RESMGR_OCB_T` defaults to `void`. | Before **any** QNX include, add: `struct _iofunc_ocb; #define RESMGR_OCB_T struct _iofunc_ocb` and `#define IOFUNC_OCB_T struct _iofunc_ocb` (top of `tickdev.c`). |
| `conflicting types for 'msg_t'` | 8.0 headers already define `msg_t`. | Don't name your own message type `msg_t`. qsentry renamed it `qs_rxmsg_t`. |
| `serial.log` is **0 bytes**, and QEMU prints `WHPX: ... Ignoring request for interrupt vector 0` | With Windows Hypervisor Platform (`-accel whpx`), the QNX 8.0 kernel never got to the console on our PC. | Use `-accel tcg -cpu max`. It is slower, so allow ~1–2 min to boot. qsentry's script tries WHPX for a few seconds and then falls back to TCG. |
| `The process cannot access the file because it is being used by another process` | You ran `type serial.log`, or started a new run, while QEMU still held the file. | Stop QEMU first (`taskkill /im qemu-system-x86_64.exe /f`), wait a few seconds, then read the file. |
| The run ends after a few seconds, or a second run's QEMU disappears | `taskkill /im qemu-system-x86_64.exe` kills **every** QEMU, including another run's. Also, `find` in a `tasklist \| find` check can resolve to Git's Unix `find` when Git is on `PATH`. | Start QEMU with `-pidfile`, wait for and kill only that PID, and call `%SystemRoot%\System32\findstr.exe` by full path (see `run_qemu.cmd`). |
| `cat /dev/tick` never ends | The read handler ignored `ocb->offset`. | See 3.2. |
| `timer_settime` fails with `EINVAL` for a 1000 ms period | `tv_nsec` must be below 1 000 000 000. | Split the period into `tv_sec` and `tv_nsec` (see `arm_timer()`). |
| `mkqnximage: command not found` | The SDP env script was not called in this window. | `call %USERPROFILE%\qnx800\qnxsdp-env.bat` |
| Files missing from `/data` in the guest | Snippet paths used `\`, or the snippet was not in `local\snippets\`. | Use forward slashes on the host side (the script does `%VM:\=/%`), and keep the snippets under the image folder's `local\snippets\`. |

## 8. Where to go next

- Add a `devctl()` handler (`iofuncs.devctl`) for binary commands instead of
  text parsing.
- Run the timer handler at a higher priority. Create the pulse with a fixed
  priority instead of `SIGEV_PULSE_PRIO_INHERIT`, and compare `max_jitter_us`
  while `qsload` (from qsentry) keeps the CPU busy.
- Look at [`../src/qnx/qsentryd.c`](../src/qnx/qsentryd.c) to see the same
  pattern scaled up. It is a resource manager plus a pulse-driven fault detector
  for vehicle sensor streams.

---

Licence: Apache-2.0, like the rest of this repository.
