#!/usr/bin/env python3
"""Removes placeholder definitions from a generated stub file.

    drop_stubs.py FILE PREFIX [PREFIX...]

Each function (or variable) whose name starts with one of the prefixes is
deleted, so that a real implementation can take its place.
"""

import re
import sys
from pathlib import Path


def main() -> None:
    path = Path(sys.argv[1])
    prefixes = sys.argv[2:]
    text = path.read_text(encoding="utf-8")
    # A definition starts at the beginning of a line and ends at the first
    # line holding only a closing brace (or at the semicolon for a variable).
    pattern = re.compile(
        r"^[^\n#/][^\n]*?\b(\w+)\s*(?:\((?:[^()]|\([^()]*\))*\)\s*\{.*?^\}\n|\[[^\]]*\];\n|;\n)\n?",
        re.M | re.S,
    )
    removed = []

    def drop(match: re.Match) -> str:
        if any(match.group(1).startswith(p) for p in prefixes):
            removed.append(match.group(1))
            return ""
        return match.group(0)

    text = pattern.sub(drop, text)
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"removed {len(removed)}: {' '.join(removed)}")


if __name__ == "__main__":
    main()
