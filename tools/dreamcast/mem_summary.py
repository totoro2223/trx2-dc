#!/usr/bin/env python3
"""Summarises a TRX Dreamcast serial log, one row per level load.

    py tools/dreamcast/mem_summary.py LOG [LOG ...]
    py tools/dreamcast/mem_summary.py LOG --ram-end 0x8D000000

The log is the file run_flycast.ps1 leaves (build/dc-serial.txt or the -Out
one). It reads what the level region, Lua, the newlib heap, the PVR audits and
the test hooks print, and marks the first failure. The output is Markdown, to
paste into REGISTRO.md or a report.

What it reads (see TRASPASO.md, "Diagnóstico"):
  game heap: / game heap after the injections:  (game/level/pipeline.c)
  [watchdog]                                    (dc_watchdog.c)
  [test] load N of K                            (dc_watchdog.c)
  [levels] on to level N / done                 (game/phase/executor.c)
  [kept] / [heap] (only with test loads)        (dc_alloc_trace.c)
  [bins] / [audit]                              (render/pvr.c)
  "[crumb]" and the level's number and path     (dc_strict.c, pipeline.c)
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
DEFAULT_RAM_END = 0x8D000000

LEVEL_RE = re.compile(r"Level_Pipeline_Load\]\s+(\d+)\s+\(([^)]*)\)")
HEAP_RE = re.compile(
    r"game heap:\s+(\d+)K,\s+(\d+)K in (\d+) blocks, largest free (\d+)K, "
    r"peak (\d+)K, (\d+) spilled"
)
INJECT_RE = re.compile(
    r"game heap after the injections:\s+(\d+)K in (\d+) blocks, "
    r"largest free (\d+)K"
)
WATCHDOG_RE = re.compile(
    r"\[watchdog\] heap=(\d+)K used=(\d+)K lua=(\d+)K "
    r"\(small=(\d+)K big=(\d+)K spilled=(\d+)\) brk=([0-9a-fA-F]+) "
    r"meshes=(\d+)/(\d+) stack=(\d+)K"
)
TEST_LOAD_RE = re.compile(
    r"\[test\] load (\d+) of (\d+): used=(\d+)K free=(\d+)K lua=(-?\d+)K "
    r"small=(\d+)K big=(\d+)K spilled=(\d+) brk=([0-9a-fA-F]+)"
)
KEPT_RE = re.compile(
    r"\[kept\]\s+(.*?):\s+(\d+) blocks,\s+(\d+)K above ([0-9a-fA-F]+)"
    r"(?:\s+\((\d+) not followed\))?"
)
HEAPMAP_RE = re.compile(
    r"\[heap\]\s+(.*?):\s+([0-9a-fA-F]+)-([0-9a-fA-F]+), "
    r"largest hole (\d+)K"
)
BINS_RE = re.compile(
    r"\[bins\] pointers per tile \(blocks OP (\d+) TR (\d+) PT (\d+)\): "
    r"OP max (\d+) over (\d+), TR max (\d+) over (\d+), "
    r"PT max (\d+) over (\d+); strips OP (\d+) TR (\d+) PT (\d+)"
)
AUDIT_VRAM_RE = re.compile(
    r"\[audit\] video memory free (\d+) KiB; lists written "
    r"OP (\d+) TR (\d+) PT (\d+) KiB"
)
AUDIT_VERTS_RE = re.compile(
    r"\[audit\] vertices the chip may hang on \(OP/TR/PT\): not finite "
    r"(\d+)/(\d+)/(\d+), depth not over 0 (\d+)/(\d+)/(\d+), "
    r"off the screen (\d+)/(\d+)/(\d+)"
)
VRAM_RE = re.compile(r"(\d+) KiB of video memory free")
CRUMB_RE = re.compile(r"\[crumb\]\s+(.+)")
LEVELS_RE = re.compile(r"\[levels\] (on to level (\d+)|done)")
BUILD_RE = re.compile(r"DC build (\d+)")
FAIL_RE = re.compile(
    r"exception [0-9a-fA-F]{4}|Assertion|aborting|starting the disc over|"
    r"Out of memory|could not be copied to the memory card|"
    r"Flycast has stopped|FATAL|Fatal"
)
OOM_RE = re.compile(r"Out of memory")


@dataclass
class Load:
    number: int
    level: str
    size_kb: int
    used_kb: int
    blocks: int
    largest_kb: int
    peak_kb: int
    spilled: int
    inject_kb: int | None = None
    inject_blocks: int | None = None
    inject_free_kb: int | None = None
    lua_kb: int | None = None
    lua_small_kb: int | None = None
    lua_big_kb: int | None = None
    lua_spilled: int | None = None
    brk: int | None = None
    vram_kb: int | None = None
    kept_blocks: int | None = None
    kept_kb: int | None = None
    heap_hole_kb: int | None = None
    flags: list[str] = field(default_factory=list)


@dataclass
class Report:
    path: Path
    build: int | None = None
    loads: list[Load] = field(default_factory=list)
    test_loads: list[dict] = field(default_factory=list)
    levels_done: bool = False
    watched: list[dict] = field(default_factory=list)
    bins: list[dict] = field(default_factory=list)
    audit: list[dict] = field(default_factory=list)
    audit_verts: list[dict] = field(default_factory=list)
    failures: list[tuple[int, str]] = field(default_factory=list)
    sbrk_probes: int = 0
    last_crumb: str | None = None
    last_vram_kb: int | None = None
    lines: int = 0


def strip_ansi(text: str) -> str:
    return ANSI.sub("", text)


def parse(path: Path, ram_end: int) -> Report:
    report = Report(path=path)
    current: Load | None = None
    last_level = "?"
    text = path.read_text(encoding="utf-8", errors="replace")
    for lineno, raw in enumerate(text.splitlines(), 1):
        report.lines = lineno
        line = strip_ansi(raw)
        if not line.strip():
            continue

        m = BUILD_RE.search(line)
        if m:
            report.build = int(m.group(1))

        m = CRUMB_RE.search(line)
        if m:
            report.last_crumb = m.group(1).strip()

        m = LEVEL_RE.search(line)
        if m:
            last_level = m.group(2)
            continue

        m = HEAP_RE.search(line)
        if m:
            current = Load(
                number=len(report.loads) + 1,
                level=last_level,
                size_kb=int(m.group(1)),
                used_kb=int(m.group(2)),
                blocks=int(m.group(3)),
                largest_kb=int(m.group(4)),
                peak_kb=int(m.group(5)),
                spilled=int(m.group(6)),
            )
            report.loads.append(current)
            continue

        m = INJECT_RE.search(line)
        if m and current is not None:
            current.inject_kb = int(m.group(1))
            current.inject_blocks = int(m.group(2))
            current.inject_free_kb = int(m.group(3))
            continue

        m = WATCHDOG_RE.search(line)
        if m:
            brk = int(m.group(7), 16)
            watch = {
                "heap_kb": int(m.group(1)),
                "used_kb": int(m.group(2)),
                "lua_kb": int(m.group(3)),
                "small_kb": int(m.group(4)),
                "big_kb": int(m.group(5)),
                "spilled": int(m.group(6)),
                "brk": brk,
                "to_end_kb": max(0, (ram_end - brk) // 1024),
                "meshes": m.group(8),
                "meshes_total": m.group(9),
                "stack_kb": int(m.group(10)),
            }
            report.watched.append(watch)
            if current is not None and current.lua_kb is None:
                current.lua_kb = watch["lua_kb"]
                current.lua_small_kb = watch["small_kb"]
                current.lua_big_kb = watch["big_kb"]
                current.lua_spilled = watch["spilled"]
                current.brk = brk
            continue

        m = TEST_LOAD_RE.search(line)
        if m:
            report.test_loads.append(
                {
                    "index": int(m.group(1)),
                    "total": int(m.group(2)),
                    "used_kb": int(m.group(3)),
                    "free_kb": int(m.group(4)),
                    "lua_kb": int(m.group(5)),
                    "small_kb": int(m.group(6)),
                    "big_kb": int(m.group(7)),
                    "spilled": int(m.group(8)),
                    "brk": int(m.group(9), 16),
                    "to_end_kb": max(
                        0, (ram_end - int(m.group(9), 16)) // 1024
                    ),
                }
            )
            continue

        m = KEPT_RE.search(line)
        if m and current is not None:
            current.kept_blocks = int(m.group(2))
            current.kept_kb = int(m.group(3))
            continue

        m = HEAPMAP_RE.search(line)
        if m and current is not None:
            current.heap_hole_kb = int(m.group(4))
            continue

        m = BINS_RE.search(line)
        if m:
            report.bins.append(
                {
                    "op_block": int(m.group(1)),
                    "tr_block": int(m.group(2)),
                    "pt_block": int(m.group(3)),
                    "op_max": int(m.group(4)),
                    "op_over": int(m.group(5)),
                    "tr_max": int(m.group(6)),
                    "tr_over": int(m.group(7)),
                    "pt_max": int(m.group(8)),
                    "pt_over": int(m.group(9)),
                    "strips": (int(m.group(10)), int(m.group(11)), int(m.group(12))),
                }
            )
            continue

        m = AUDIT_VERTS_RE.search(line)
        if m:
            report.audit_verts.append(
                {
                    "not_finite": tuple(int(m.group(i)) for i in (1, 2, 3)),
                    "no_depth": tuple(int(m.group(i)) for i in (4, 5, 6)),
                    "far_off": tuple(int(m.group(i)) for i in (7, 8, 9)),
                }
            )
            continue

        m = AUDIT_VRAM_RE.search(line)
        if m:
            report.audit.append(
                {
                    "vram_kb": int(m.group(1)),
                    "lists_kb": (int(m.group(2)), int(m.group(3)), int(m.group(4))),
                }
            )
            report.last_vram_kb = int(m.group(1))
            if current is not None and current.vram_kb is None:
                current.vram_kb = int(m.group(1))
            continue

        m = VRAM_RE.search(line)
        if m:
            report.last_vram_kb = int(m.group(1))
            continue

        m = LEVELS_RE.search(line)
        if m:
            if m.group(1) == "done":
                report.levels_done = True
            continue

        if "Out of memory. Requested sbrk_base" in line:
            # newlib tries the whole heap in one go and walks its request
            # down: the level region's own growth, not a failure.
            report.sbrk_probes += 1
            continue

        if FAIL_RE.search(line):
            report.failures.append((lineno, line.strip()))

    # Attach the last known brk/Lua/VRAM to loads that had none at load time.
    for load in report.loads:
        if load.lua_kb is None and report.watched:
            # The first watchdog after this load, if any, is already in the
            # list; fall back to the last one for logs cut short.
            after = [w for w in report.watched if w["brk"] >= (load.brk or 0)]
            if after:
                w = after[0]
                load.lua_kb = w["lua_kb"]
                load.lua_small_kb = w["small_kb"]
                load.lua_big_kb = w["big_kb"]
                load.lua_spilled = w["spilled"]
                load.brk = w["brk"]
    return report


def kb(value: int | None) -> str:
    return "—" if value is None else f"{value}K"


def to_markdown(report: Report, ram_end: int) -> str:
    out: list[str] = []
    out.append(f"# Memoria — `{report.path.name}`")
    out.append("")
    out.append(
        f"Build: **{report.build or '?'}** · líneas: {report.lines} · "
        f"cargas: {len(report.loads)} · "
        f"`[levels] done`: {'sí' if report.levels_done else 'no'}"
    )
    if report.last_crumb:
        out.append(f"Última miga: `{report.last_crumb}`")
    if report.sbrk_probes:
        out.append(
            f"`sbrk` rechazados antes de crecer (normal): {report.sbrk_probes}"
        )
    if report.last_vram_kb is not None:
        out.append(f"VRAM libre (último aviso): **{report.last_vram_kb} KiB**")
    out.append("")

    out.append("## Cargas")
    out.append("")
    out.append(
        "| # | Nivel | Región | Usado al entrar | Hueco mayor | Bloques | "
        "Tras inyecciones | Pico | Spilled | Lua | brk | A fin RAM | VRAM | Avisos |"
    )
    out.append(
        "|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|---|"
    )
    for load in report.loads:
        to_end = (
            "—" if load.brk is None else f"{max(0, (ram_end - load.brk) // 1024)}K"
        )
        level = load.level
        if len(level) > 46:
            level = "…" + level[-45:]
        inject = "—"
        if load.inject_kb is not None:
            inject = f"{load.inject_kb}K/{load.inject_blocks}b"
        flags = ", ".join(load.flags) if load.flags else ""
        out.append(
            f"| {load.number} | `{level}` | {load.size_kb}K | "
            f"{load.used_kb}K | {load.largest_kb}K | {load.blocks} | {inject} | "
            f"{load.peak_kb}K | {load.spilled} | {kb(load.lua_kb)} | "
            f"{'—' if load.brk is None else hex(load.brk)} | {to_end} | "
            f"{kb(load.vram_kb)} | {flags} |"
        )
    out.append("")

    if report.test_loads:
        out.append("## Cargas de partida en serie (`[test] load`)")
        out.append("")
        out.append("| # | Usado | Libre | Lua | small | big | spilled | brk | A fin RAM |")
        out.append("|---:|---:|---:|---:|---:|---:|---:|---|---:|")
        for t in report.test_loads:
            out.append(
                f"| {t['index']}/{t['total']} | {t['used_kb']}K | "
                f"{t['free_kb']}K | {t['lua_kb']}K | {t['small_kb']}K | "
                f"{t['big_kb']}K | {t['spilled']} | {hex(t['brk'])} | "
                f"{t['to_end_kb']}K |"
            )
        out.append("")

    if report.watched:
        first = report.watched[0]
        last = report.watched[-1]
        lows = min(w["to_end_kb"] for w in report.watched)
        out.append("## Tendencias")
        out.append("")
        out.append(
            f"- `[watchdog]`: {len(report.watched)} muestras; "
            f"`brk` mínimo a **{lows}K** del final de la RAM."
        )
        out.append(
            f"- Primera muestra: used {first['used_kb']}K, Lua {first['lua_kb']}K, "
            f"spilled {first['spilled']}."
        )
        out.append(
            f"- Última muestra: used {last['used_kb']}K, Lua {last['lua_kb']}K, "
            f"spilled {last['spilled']}."
        )
        if len(report.loads) >= 2:
            loaded = [load for load in report.loads if load.largest_kb > 0]
            if len(loaded) >= 2:
                first_load = loaded[0]
                last_load = loaded[-1]
                delta = last_load.largest_kb - first_load.largest_kb
                lows = min(load.largest_kb for load in loaded)
                out.append(
                    f"- Hueco mayor entre la primera y la última carga con "
                    f"datos: {first_load.largest_kb}K → "
                    f"{last_load.largest_kb}K ({delta:+d}K); mínimo "
                    f"{lows}K."
                )
                out.append(
                    f"- Región del nivel: {first_load.size_kb}K → "
                    f"{last_load.size_kb}K; en uso {first_load.used_kb}K → "
                    f"{last_load.used_kb}K."
                )
        if any(load.kept_kb is not None for load in report.loads):
            kept = [load for load in report.loads if load.kept_kb is not None]
            out.append(
                "- Retenido entre niveles (`[kept]`): "
                + ", ".join(
                    f"{load.number}: {load.kept_kb}K/{load.kept_blocks}b"
                    for load in kept[-5:]
                )
                + "."
            )
        if any(w["spilled"] for w in report.watched):
            out.append("- **Hay `spilled` en la región de Lua.**")
        out.append("")

    if report.bins:
        last = report.bins[-1]
        out.append("## Auditoría PVR")
        out.append("")
        names = ("OP", "TR", "PT")
        block = (last["op_block"], last["tr_block"], last["pt_block"])
        maxima = (
            max(b["op_max"] for b in report.bins),
            max(b["tr_max"] for b in report.bins),
            max(b["pt_max"] for b in report.bins),
        )
        worst_over = (
            max(b["op_over"] for b in report.bins),
            max(b["tr_over"] for b in report.bins),
            max(b["pt_over"] for b in report.bins),
        )
        out.append(
            f"- `[bins]`: {len(report.bins)} muestras; bloques "
            + ", ".join(f"{n} {v}" for n, v in zip(names, block))
            + "; máximo de punteros "
            + ", ".join(f"{n} {v}" for n, v in zip(names, maxima))
            + "."
        )
        bad = [
            f"{n} {o} teselas (máx {m}, bloque {b})"
            for n, m, o, b in zip(names, maxima, worst_over, block)
            if o > 0
        ]
        if bad:
            out.append(
                "- **Desborde de punteros en "
                + "; ".join(bad)
                + ".** El chip lee de más y la tesela puede salir mal."
            )
        else:
            out.append("- Sin desborde de punteros en ninguna muestra.")
        if report.audit_verts:
            v = report.audit_verts
            out.append(
                "- `[audit]` vértices: no finitos "
                + "/".join(str(max(x["not_finite"][i] for x in v)) for i in range(3))
                + ", sin profundidad "
                + "/".join(str(max(x["no_depth"][i] for x in v)) for i in range(3))
                + ", fuera de pantalla "
                + "/".join(str(max(x["far_off"][i] for x in v)) for i in range(3))
                + "."
            )
        if report.audit:
            a = report.audit[-1]
            out.append(
                f"- `[audit]` final: VRAM libre {a['vram_kb']} KiB; listas "
                f"OP {a['lists_kb'][0]}K, TR {a['lists_kb'][1]}K, "
                f"PT {a['lists_kb'][2]}K."
            )
        out.append("")

    out.append("## Fallos")
    out.append("")
    if report.failures:
        for lineno, line in report.failures[:20]:
            out.append(f"- línea {lineno}: `{line}`")
        if len(report.failures) > 20:
            out.append(f"- … y {len(report.failures) - 20} más.")
    else:
        out.append("- Ninguno de los patrones de fallo.")
    out.append("")
    return "\n".join(out)


def main() -> None:
    # The Windows console is cp1252 and the tables use arrows and ellipses.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument(
        "--ram-end",
        type=lambda s: int(s, 0),
        default=DEFAULT_RAM_END,
        help="dirección del final de la RAM (por defecto 0x8D000000)",
    )
    parser.add_argument(
        "-o", "--output", type=Path, help="escribir el Markdown aquí"
    )
    args = parser.parse_args()

    chunks = []
    for path in args.logs:
        if not path.is_file():
            sys.exit(f"no existe: {path}")
        chunks.append(to_markdown(parse(path, args.ram_end), args.ram_end))
    text = "\n".join(chunks)
    if args.output:
        args.output.write_text(text, encoding="utf-8")
        print(f"escrito {args.output}")
    else:
        print(text)


if __name__ == "__main__":
    main()
