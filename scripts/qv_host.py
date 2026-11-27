#!/usr/bin/env python3
"""The host end of the qvcan UDP bridge (tools/vcanbridge.c in the guest).

Every frame that arrives is written to the log. Frames with a standard id in
0x300..0x3FF get an answer: id + 0x100 and the data bytes reversed, which is
what "vcan_test bridge" in the guest waits for. Nothing else is answered.

    python scripts/qv_host.py [--bind 127.0.0.1:47002] [--peer 127.0.0.1:47001] [--log FILE]

scripts/qv.py run starts serve() in a thread while QEMU runs.

Datagram: b"QVB1", count (uint16, big endian), 0 (uint16), then count records
of 16 bytes: id with flags (uint32, big endian), len, 0, 0, 0, data[8].
Standard library only.
SPDX-License-Identifier: Apache-2.0
"""
import argparse
import socket
import struct
import threading
import time

MAGIC = b"QVB1"
EFF_FLAG = 0x80000000
RTR_FLAG = 0x40000000
ERR_FLAG = 0x20000000
MAXREC = 64


def decode(pkt):
    """Frames in a datagram as (id_with_flags, data bytes), or None if malformed."""
    if len(pkt) < 8 or pkt[:4] != MAGIC:
        return None
    count = struct.unpack(">H", pkt[4:6])[0]
    if count > MAXREC or len(pkt) < 8 + 16 * count:
        return None
    frames = []
    for i in range(count):
        rec = pkt[8 + 16 * i: 24 + 16 * i]
        fid = struct.unpack(">I", rec[:4])[0]
        n = min(rec[4], 8)
        frames.append((fid, bytes(rec[8:8 + n])))
    return frames


def encode(frames):
    out = bytearray(MAGIC + struct.pack(">HH", len(frames), 0))
    for fid, data in frames:
        out += struct.pack(">IB3x", fid, len(data)) + bytes(data).ljust(8, b"\0")
    return bytes(out)


def answer(fid, data):
    """The reply to one frame, or None."""
    if fid & (EFF_FLAG | RTR_FLAG | ERR_FLAG):
        return None
    if 0x300 <= fid <= 0x3FF:
        return fid + 0x100, data[::-1]
    return None


def fmt(fid, data):
    if fid & ERR_FLAG:
        ident = "ERR"
    elif fid & EFF_FLAG:
        ident = "%08X" % (fid & 0x1FFFFFFF)
    else:
        ident = "%03X" % (fid & 0x7FF)
    return "%s [%d] %s" % (ident, len(data), data.hex(" ").upper())


def serve(bind_ip, bind_port, peer_ip, peer_port, log_path, stop_event):
    """Answer frames until stop_event is set; counts at the end of the log."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind_ip, bind_port))
    sock.settimeout(0.5)
    t0 = time.time()
    got = sent = bad = 0
    with open(log_path, "w", encoding="utf-8", newline="\n") as log:
        log.write("qv_host: listening on udp %s:%d, answering to %s:%d\n"
                  % (bind_ip, bind_port, peer_ip, peer_port))
        log.flush()
        while not stop_event.is_set():
            try:
                pkt, _src = sock.recvfrom(8 + 16 * MAXREC + 64)
            except socket.timeout:
                continue
            except OSError as e:  # e.g. ICMP port unreachable on Windows
                log.write("%9.3f  socket: %s\n" % (time.time() - t0, e))
                continue
            frames = decode(pkt)
            if frames is None:
                bad += 1
                log.write("%9.3f  bad datagram of %d bytes\n" % (time.time() - t0, len(pkt)))
                continue
            replies = []
            for fid, data in frames:
                got += 1
                log.write("%9.3f  rx %s\n" % (time.time() - t0, fmt(fid, data)))
                r = answer(fid, data)
                if r:
                    replies.append(r)
                    log.write("%9.3f  tx %s\n" % (time.time() - t0, fmt(*r)))
            if replies:
                try:
                    sock.sendto(encode(replies), (peer_ip, peer_port))
                    sent += len(replies)
                except OSError as e:
                    log.write("%9.3f  send: %s\n" % (time.time() - t0, e))
            log.flush()
        log.write("qv_host END received=%d answered=%d bad=%d\n" % (got, sent, bad))
    sock.close()


def selftest():
    frames = [(0x305, b"\x01\x02\x03\x04"), (EFF_FLAG | 0x12345678, b""), (0x7FF, b"12345678")]
    assert decode(encode(frames)) == frames
    assert answer(0x305, b"\x01\x02\x03\x04") == (0x405, b"\x04\x03\x02\x01")
    assert answer(0x2FF, b"") is None and answer(EFF_FLAG | 0x305, b"") is None
    assert decode(b"QVB1\x00\x02\x00\x00" + b"\0" * 16) is None
    assert decode(b"XXXX\x00\x00\x00\x00") is None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bind", default="127.0.0.1:47002")
    ap.add_argument("--peer", default="127.0.0.1:47001")
    ap.add_argument("--log", default="host_bridge.txt")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        print("qv_host selftest: ok")
        return
    bip, bport = a.bind.rsplit(":", 1)
    pip, pport = a.peer.rsplit(":", 1)
    stop = threading.Event()
    try:
        serve(bip, int(bport), pip, int(pport), a.log, stop)
    except KeyboardInterrupt:
        stop.set()


if __name__ == "__main__":
    main()
