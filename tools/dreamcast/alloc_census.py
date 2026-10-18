#!/usr/bin/env python3
"""Sums the large allocations traced on the serial console by the function
that asked for them (the first frame that is not an allocator).

    alloc_census.py SERIAL_LOG TRX.elf ADDR2LINE [START_MARKER]

Only allocations after the line holding START_MARKER count. These are
allocations, not what is held: what was freed again is counted too.
"""

import re
import subprocess
import sys
from collections import defaultdict

SKIP = re.compile(
    r"^(__wrap_|Memory_|Vector_|File_|M_Buffer|M_Arena|malloc|calloc|realloc|"
    r"Benchmark_|SDL_|mutex_|\?\?)"
)


def main() -> None:
    log, elf, addr2line = sys.argv[1:4]
    marker = sys.argv[4] if len(sys.argv) > 4 else None
    text = open(log, encoding="utf-8", errors="replace").read()
    if marker and marker in text:
        text = text[text.index(marker):]
    text = text.replace("\n", "")
    records = re.findall(r"\[alloc\] \w+ (\d+)K -> 0x\w+ from \w+ \(in use \d+K\) trace:((?: [0-9a-f]{8})+)", text)
    addresses = sorted({a for _, t in records for a in t.split()})
    out = subprocess.run(
        [addr2line, "-f", "-e", elf, *addresses],
        capture_output=True, text=True, check=True,
    ).stdout.splitlines()
    names = {addr: out[i * 2] for i, addr in enumerate(addresses)}
    totals = defaultdict(int)
    counts = defaultdict(int)
    for size, trace in records:
        owner = "?"
        for addr in trace.split():
            name = names.get(addr, "??")
            if not SKIP.match(name):
                owner = name
                break
        totals[owner] += int(size)
        counts[owner] += 1
    for owner, total in sorted(totals.items(), key=lambda kv: -kv[1])[:30]:
        print(f"{total:7d}K {counts[owner]:4d}x {owner}")


if __name__ == "__main__":
    main()
