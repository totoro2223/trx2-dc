#!/usr/bin/env python3
"""Turns the sampling profiler's "[prof]" lines from the serial console into
time per function.

    prof_report.py SERIAL_LOG TRX.elf ADDR2LINE [BLOCKS]

Sums the last BLOCKS dumps (default: all of them).
"""

import re
import subprocess
import sys
from collections import defaultdict


def main() -> None:
    log, elf, addr2line = sys.argv[1:4]
    want = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    text = open(log, encoding="utf-8", errors="replace").read()
    blocks = re.findall(r"\[prof\] begin.*?\[prof\] end", text, re.S)
    if want > 0:
        blocks = blocks[-want:]
    counts = defaultdict(int)
    for block in blocks:
        for pc, count in re.findall(r"\[prof\] ([0-9a-f]{8}) (\d+)", block):
            counts[pc] += int(count)
    if not counts:
        print("no samples")
        return
    pcs = sorted(counts)
    out = subprocess.run(
        [addr2line, "-f", "-e", elf, *pcs],
        capture_output=True, text=True, check=True,
    ).stdout.splitlines()
    by_func = defaultdict(int)
    for i, pc in enumerate(pcs):
        by_func[out[i * 2]] += counts[pc]
    total = sum(counts.values())
    print(f"{len(blocks)} dumps, {total} samples")
    for func, count in sorted(by_func.items(), key=lambda kv: -kv[1])[:40]:
        print(f"{100.0 * count / total:5.1f}% {count:6d} {func}")


if __name__ == "__main__":
    main()
