# Windows host: build a QNX 8.0 QEMU image and run the scripted demo

Tested with QNX SDP 8.0 (Windows host, `qcc` 12.2.0) and `mkqnximage`.

1. `scripts\build_qnx.cmd x86_64`
2. `scripts\qemu_win\run_qemu_win.cmd` — copies the binaries into a VM folder
   (`%USERPROFILE%\qsentry-vm`), generates `local\snippets\data_files.custom`,
   builds the disk with `mkqnximage --type=qemu --arch=x86_64`, boots it in QEMU
   (no networking needed) and writes the serial console to `serial.log`.
   `scripts/autodemo.sh` runs at boot: start `qsentryd`/`qsensord`, inject a yaw
   drift, reset, run `qsload` at low priority, then `slay qsensord` to trip the watchdog.

Requires QEMU for Windows (`winget install SoftwareFreedomConservancy.QEMU`).
