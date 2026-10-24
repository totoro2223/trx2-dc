#!/usr/bin/env python3
"""Sums what the level heap holds at each level load, by the function that
asked for it (DC_LevelHeap_DumpLive's "[live]" lines in the serial log).

    live_census.py SERIAL_LOG TRX.elf ADDR2LINE [DUMP_INDEX]

Prints, for the dump asked for (the last by default), the sizes by owner, and
what the dump holds that the one before it did not.
"""

import re
import subprocess
import sys
from collections import defaultdict

SKIP = re.compile(
    r"^(__wrap_|Memory_|DC_LevelHeap|M_Take|M_Realloc|malloc|calloc|realloc|"
    r"\?\?|Vector_Add|Vector_Expand|Vector_Create|String_|strdup)"
)


def dumps(text):
    out = []
    for block in re.split(r"\[live-begin\][^\n]*\n", text)[1:]:
        block = block.split("[live-end]")[0]
        rows = []
        for line in block.splitlines():
            m = re.match(r"\[live\] (\d+)((?: [0-9a-f]{8})+)", line.strip())
            if m:
                rows.append((int(m.group(1)), m.group(2).split()))
        out.append(rows)
    return out


def main():
    log, elf, addr2line = sys.argv[1:4]
    text = open(log, encoding="utf-8", errors="replace").read()
    text = re.sub(r"\x1b\[[0-9;]*m", "", text)
    ds = dumps(text)
    index = int(sys.argv[4]) if len(sys.argv) > 4 else len(ds) - 1
    print("dumps:", [len(d) for d in ds], "using", index)
    addrs = sorted({a for d in ds for _, t in d for a in t})
    res = subprocess.run(
        [addr2line, "-f", "-e", elf, *addrs], capture_output=True, text=True
    ).stdout.splitlines()
    names = {a: res[i * 2] for i, a in enumerate(addrs)}

    def owner(trace):
        chain = []
        for a in trace:
            n = names.get(a, "?")
            if SKIP.match(n):
                continue
            chain.append(n)
            if len(chain) == 3:
                break
        return " < ".join(chain) if chain else "?"

    def tally(d):
        by = defaultdict(lambda: [0, 0])
        for size, trace in d:
            o = owner(trace)
            by[o][0] += size
            by[o][1] += 1
        return by

    now = tally(ds[index])
    before = tally(ds[index - 1]) if index > 0 else {}
    print("%8s %5s  %s" % ("bytes", "n", "owner (new since the dump before)"))
    for o, (size, n) in sorted(now.items(), key=lambda kv: -kv[1][0])[:30]:
        old = before.get(o, [0, 0])
        print("%8d %5d  %s   (+%d, +%d)" % (size, n, o, size - old[0], n - old[1]))


main()
