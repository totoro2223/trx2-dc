#!/usr/bin/env python3

"""Embed the engine's Lua into C, as the two arrays declared in
trx/game/lua/embedded_scripts.h. Which array a file lands in is decided by the
meson source list it is named in, not by anything the loader has to work out.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from shared.luaannot import (  # noqa: E402
    annotated_modules,
    parse_files,
    signatures,
    signatures_module,
    to_lua,
)

Script = tuple[str, str, bytes]


def _c_ident_from_path(path: Path, taken: set[str]) -> str:
    raw = path.as_posix()
    out: list[str] = []
    for ch in raw:
        if ch.isalnum():
            out.append(ch)
        else:
            out.append("_")
    # The path is now part of the name, and every character that is not
    # alphanumeric folds to an underscore, so a/b.lua and a-b.lua arrive here as
    # the same identifier.
    base = "m_TrxLua_" + "".join(out)
    ident = base
    suffix = 2
    while ident in taken:
        ident = f"{base}_{suffix}"
        suffix += 1
    taken.add(ident)
    return ident


def _bytes_to_c_array(data: bytes) -> str:
    if not data:
        # An empty initialiser is not C. The loader reads the size, which is zero.
        return "    0"
    per_line = 16
    chunks: list[str] = []
    for i in range(0, len(data), per_line):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i : i + per_line])
        chunks.append("    " + chunk)
    return ",\n".join(chunks)


def _collect(paths: list[Path], root: Path, taken: set[str]) -> list[Script]:
    # A module's path is what its name is derived from, so it is kept bare:
    # items.lua, the module trx.items. A script's keeps its directory.
    scripts: list[Script] = []
    for path in paths:
        rel = path.resolve().relative_to(root.resolve())
        scripts.append(
            (rel.as_posix(), _c_ident_from_path(rel, taken), path.read_bytes())
        )
    return scripts


def _lean_signatures(root: Path) -> str:
    # Only what the modules are built against (members) and what type checking
    # looks names up in. The rest serves strict mode and the seal's audits,
    # which a game that never turns strict mode on and seals partially does
    # without.
    full = signatures(parse_files(annotated_modules(root)))
    kept = ("members", "records", "types")
    lean = {k: v if k in kept else {} for k, v in full.items()}
    return f"return {to_lua(lean)}\n"


def _compile(scripts: list[Script], luac: Path) -> list[Script]:
    # Stripped bytecode in place of the source: no parsing on the target, and
    # none of the debug information it would keep. The target loads it from
    # where it lies (a fixed buffer), so the arrays are aligned.
    compiled: list[Script] = []
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "in.lua"
        out = Path(tmp) / "out.luac"
        for path, c_ident, data in scripts:
            src.write_bytes(data)
            subprocess.run(
                [str(luac), "-s", "-o", str(out), str(src)], check=True
            )
            compiled.append((path, c_ident, out.read_bytes()))
    return compiled


def _emit_array(
    name: str, scripts: list[Script], offsets: dict[str, int] | None
) -> list[str]:
    lines = [f"const LUA_EMBEDDED_SCRIPT {name}[] = {{"]
    for path, c_ident, data in scripts:
        if offsets is not None:
            lines.append(
                f'    {{ .path = "{path}", .offset = {offsets[path]}, '
                f".size = {len(data)} }},"
            )
        else:
            lines.append(
                f'    {{ .path = "{path}", .data = {c_ident}, '
                f".size = {len(data)} }},"
            )
    lines.append("    { .path = nullptr, .data = nullptr, .size = 0 },")
    lines.append("};")
    lines.append("")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description="Embed TRX Lua scripts into C")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--module-root", required=True, type=Path)
    parser.add_argument("--modules", nargs="*", default=[], type=Path)
    parser.add_argument("--script-root", required=True, type=Path)
    parser.add_argument("--scripts", nargs="*", default=[], type=Path)
    parser.add_argument("--luac", type=Path)
    # The bytecode goes to this file instead of into arrays, for a target that
    # keeps it somewhere other than its main memory. Needs --luac.
    parser.add_argument("--blob", type=Path)
    parser.add_argument("--lean-signatures", action="store_true")
    args = parser.parse_args()

    taken: set[str] = set()
    modules = _collect(args.modules, args.module_root, taken)
    # trx.internal.signatures is generated from the annotations of the modules
    # above,
    # for strict mode to check calls against.
    source = (
        _lean_signatures(args.module_root)
        if args.lean_signatures
        else signatures_module(args.module_root)
    )
    modules.append(
        (
            "internal/signatures.lua",
            _c_ident_from_path(Path("internal/signatures.lua"), taken),
            source.encode("utf-8"),
        )
    )
    scripts = _collect(args.scripts, args.script_root, taken)
    if args.luac:
        modules = _compile(modules, args.luac)
        scripts = _compile(scripts, args.luac)
    align = " _Alignas(8)" if args.luac else ""

    offsets: dict[str, int] | None = None
    if args.blob:
        # The developer console's commands are not run on such a target.
        scripts = [s for s in scripts if not s[0].startswith("commands/")]
        offsets = {}
        blob = bytearray()
        for path, _, data in modules + scripts:
            blob += b"\0" * (-len(blob) % 8)
            offsets[path] = len(blob)
            blob += data
        blob += b"\0" * (-len(blob) % 4)
        args.blob.write_bytes(blob)

    lines: list[str] = []
    lines.append("// Auto-generated file; do not edit.")
    lines.append("#include <trx/game/lua/embedded_scripts.h>")
    lines.append("")
    for _, c_ident, data in [] if offsets is not None else modules + scripts:
        lines.append(f"static const uint8_t{align} {c_ident}[] = {{")
        lines.append(_bytes_to_c_array(data) + ",")
        lines.append("};")
        lines.append("")

    lines += _emit_array("g_LUA_EmbeddedModules", modules, offsets)
    lines += _emit_array("g_LUA_EmbeddedRuntimeScripts", scripts, offsets)

    args.output.write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
