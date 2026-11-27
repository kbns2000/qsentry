#!/usr/bin/env python3
"""An independent model of the CAN 2.0 frame length, to check vcan_core.c.

Written from the CAN 2.0 frame layout (SOF, arbitration field, control field,
data, CRC-15 with polynomial 0x4599, bit stuffing from SOF to the end of the
CRC, then CRC delimiter, ACK slot and delimiter, 7 EOF bits and 3 IFS bits),
not from the C code. It drives tests/host/core_test (mode "bits") with random
frames and with hand-picked ones, and fails on the first difference.

    python3 tests/host/canbits.py build/host/core_test

SPDX-License-Identifier: Apache-2.0
"""
import random
import subprocess
import sys

EFF, RTR = 0x80000000, 0x40000000


def crc15(bits):
    crc = 0
    for b in bits:
        top = (crc >> 14) & 1
        crc = (crc << 1) & 0x7FFF
        if b ^ top:
            crc ^= 0x4599
    return crc


def tobits(value, width):
    return [(value >> (width - 1 - i)) & 1 for i in range(width)]


def frame_bits(ident, data, rtr=False):
    """(total bits, nominal bits, stuff bits) of a frame"""
    if ident & EFF:
        i = ident & 0x1FFFFFFF
        s = [0] + tobits(i >> 18, 11) + [1, 1] + tobits(i & 0x3FFFF, 18) + [int(rtr), 0, 0]
    else:
        s = [0] + tobits(ident & 0x7FF, 11) + [int(rtr), 0, 0]
    s += tobits(len(data), 4)
    if not rtr:
        for byte in data:
            s += tobits(byte, 8)
    s += tobits(crc15(s), 15)
    # stuffing: after five equal bits comes one of the other value, and that
    # bit counts for the next run
    out = []
    run, prev = 0, None
    for b in s:
        out.append(b)
        if b == prev:
            run += 1
        else:
            prev, run = b, 1
        if run == 5:
            out.append(1 - b)
            prev, run = 1 - b, 1
    stuff = len(out) - len(s)
    return len(out) + 13, len(s) + 13, stuff


def main():
    exe = sys.argv[1]
    rnd = random.Random(62541)
    cases = [
        (0x000, [0] * 8, False), (0x7FF, [0xFF] * 8, False), (0x555, [0x55] * 8, False),
        (0x123, [], False), (0x123, [], True), (0x000, [], False),
        (EFF | 0x1FFFFFFF, [0xFF] * 8, False), (EFF | 0, [0] * 8, False),
        (0x0F0, [0x0F, 0xF0, 0x0F, 0xF0], False),
    ]
    for _ in range(3000):
        eff = rnd.random() < 0.3
        ident = (EFF | rnd.getrandbits(29)) if eff else rnd.getrandbits(11)
        n = rnd.randint(0, 8)
        pick = rnd.random()
        if pick < 0.2:
            data = [0] * n
        elif pick < 0.3:
            data = [0xFF] * n
        else:
            data = [rnd.getrandbits(8) for _ in range(n)]
        cases.append((ident, data, rnd.random() < 0.05))
    lines = []
    for ident, data, rtr in cases:
        i = ident | (RTR if rtr else 0)
        lines.append(f"{i:08x} {len(data)} {''.join(f'{b:02x}' for b in data) or '-'}")
    p = subprocess.run([exe, "bits"], input="\n".join(lines) + "\n",
                       capture_output=True, text=True, check=True)
    got = p.stdout.split("\n")
    worst = {}
    for k, ((ident, data, rtr), line) in enumerate(zip(cases, got)):
        want = frame_bits(ident, data, rtr)
        have = tuple(int(x) for x in line.split()[:3])
        if have[:2] != want[:2]:
            sys.exit(f"FAIL frame {k} id {ident:#x} data {data} rtr {rtr}: "
                     f"C says {have}, model says {want}")
        # the C side also prints the largest number of stuff bits for the kind
        # of frame; no real frame may have more
        if want[2] > have[2]:
            sys.exit(f"FAIL frame {k}: {want[2]} stuff bits, above the bound {have[2]}")
        key = ("eff" if ident & EFF else "sff", 0 if rtr else len(data))
        worst[key] = max(worst.get(key, 0), want[0])
    print(f"canbits: {len(cases)} frames, C and model agree")
    print("canbits: longest seen, standard 8 bytes:", worst.get(("sff", 8)),
          "bits; extended 8 bytes:", worst.get(("eff", 8)), "bits")


if __name__ == "__main__":
    main()
