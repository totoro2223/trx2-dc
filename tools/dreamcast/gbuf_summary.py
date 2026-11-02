#!/usr/bin/env python3
"""Tabulates what each level is made of, by game buffer (GameBuf_DumpSizes's
"[gbuf]" lines in the serial log), in K per level.

    gbuf_summary.py SERIAL_LOG [SERIAL_LOG ...]

The buffer names come from src/trx/game/game_buf.h.
"""

import re
import sys
from pathlib import Path

HEADER = Path(__file__).resolve().parents[2] / "src/trx/game/game_buf.h"


def names():
    text = HEADER.read_text()
    return re.findall(r"^\s+(GBUF_\w+),", text, re.M)


def loads(text):
    level = "?"
    out = []
    sizes = {}
    for line in text.splitlines():
        m = re.search(r"Level_Pipeline_Load\] \d+ \(.*?([^/]+)\)", line)
        if m:
            level = m.group(1)
        m = re.match(r".*\[gbuf\] (\d+|total) (\d+)", line)
        if not m:
            continue
        if m.group(1) == "total":
            out.append((level, sizes, int(m.group(2))))
            sizes = {}
        else:
            sizes[int(m.group(1))] = int(m.group(2))
    return out


def main():
    gbuf = names()
    for log in sys.argv[1:]:
        text = open(log, encoding="utf-8", errors="replace").read()
        text = re.sub(r"\x1b\[[0-9;]*m", "", text)
        rows = loads(text)
        if not rows:
            continue
        used = sorted(
            {i for _, s, _ in rows for i in s},
            key=lambda i: -max(s.get(i, 0) for _, s, _ in rows),
        )[:10]
        short = [gbuf[i].removeprefix("GBUF_").lower() for i in used]
        print(f"\n{log}\n")
        print("| level | total K | " + " | ".join(short) + " |")
        print("|---|---:|" + "---:|" * len(used))
        for level, sizes, total in rows:
            cells = " | ".join(str(sizes.get(i, 0) // 1024) for i in used)
            print(f"| {level} | {total // 1024} | {cells} |")


main()
