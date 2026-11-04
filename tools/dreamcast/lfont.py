#!/usr/bin/env python3
"""Makes lfont-<game>.bin (the loading screen's font) from a serial log that
holds the LFONT lines of DC_LoadingFont_Capture.

usage: lfont.py <dc-serial.txt> <out.bin> <game 1..3>
"""
import re
import struct
import sys

log, out, game = sys.argv[1], sys.argv[2], sys.argv[3]
head = {}
rows = {}
for line in open(log, errors="replace"):
    m = re.search(r"LFONT (\d) (\d+) (\d+) (\d+) (-?\d+)", line)
    if m and m[1] == game:
        head[int(m[2])] = (int(m[3]), int(m[4]), int(m[5]))
        continue
    m = re.search(r"LFONTROW (\d) (\d+) ([0-9a-f]+)", line)
    if m and m[1] == game:
        rows.setdefault(int(m[2]), []).append(m[3])
assert 33 in head, "no font lines for game %s" % game
with open(out, "wb") as f:
    for i in range(95):
        w, h, y = head.get(i, (0, 0, 0))
        hexs = "".join(rows.get(i, []))
        assert len(hexs) == w * h * 4, (i, w, h, len(hexs))
        f.write(struct.pack("BBb", w, h, y) + b"".join(
            struct.pack("<H", int(hexs[k:k + 4], 16))
            for k in range(0, len(hexs), 4)))
