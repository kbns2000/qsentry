# Results

`serial_tickdev.log` is the QNX console from `run_qemu.cmd`, run on 2026-11-08 on a
Windows 11 PC (QEMU TCG, 2 vCPUs). The text was relayed from the PC verbatim, except
that one terminal escape byte (ESC before `[?7h`) was dropped.

| line | meaning |
|---|---|
| `ticks=103 ... period_ms=10` | ~1 s after start: 103 ticks at 10 ms |
| `ticks=307 ...` | 2 s later: +204 ticks, as expected at 100 Hz |
| `ticks=514 ... period_ms=5` | 1 s after `period 5`: +207 ticks, as expected at 200 Hz |
| `ticks=3 ...` | right after `reset` |
| `max_jitter_us` 1570-2146 | worst wake-up error under **TCG software emulation**; not representative of hardware |
| `10r RECEIVE` | the daemon sleeps in `MsgReceive` between ticks and requests |
