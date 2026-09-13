#!/usr/bin/env python3
"""doctor.py -- prove a PPC32 big-endian toolchain and emit a receipt.

Compiles and links a minimal fixture, then inspects the output ELF to confirm
ELF32 / MSB / EM_PPC / real PPC instructions and RELA relocations. Writes a
toolchain receipt that build.py consumes. Never falls back to a PATH compiler:
the toolchain root is explicit (spec 5.1, 12).
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))

from gfbuild import elf as elfmod          # noqa: E402
from gfbuild import reloc as relocmod      # noqa: E402
from gfbuild.toolchain import (            # noqa: E402
    Toolchain, ToolchainReceipt, ToolchainError,
    TARGET_TRIPLE, COMMON_FLAGS, C_FLAGS, CXX_FLAGS,
)

# Fixture exercises a call (REL24), a rodata pointer (ADDR32) and a hi/lo pair.
_FIXTURE_C = r"""
extern int __attribute__((noinline)) ext_target(int);
static const int table[4] = {1, 2, 3, 4};
const int* get_table(void) { return table; }
int call_ext(int x) { return ext_target(x) + table[x & 3]; }
"""


def _probe(tc: Toolchain, workdir: Path) -> dict:
    src = workdir / "fixture.c"
    src.write_text(_FIXTURE_C)
    obj = workdir / "fixture.o"
    tc.compile_one(src, obj, "c", [], [])
    rel = workdir / "fixture.rel.o"
    tc.partial_link([obj], rel)

    parsed = elfmod.parse_elf(rel.read_bytes())
    if parsed.e_machine != elfmod.EM_PPC:
        raise ToolchainError("linked object is not EM_PPC")
    # Confirm real PPC instructions via objdump.
    cp = subprocess.run([str(tc.objdump), "-d", str(rel)],
                        capture_output=True, text=True)
    disasm = cp.stdout
    if "bl " not in disasm and "b " not in disasm:
        raise ToolchainError("disassembly shows no PPC branch; wrong backend?")
    reloc_types = sorted({r.r_type for rels in parsed.relocations.values()
                          for r in rels})
    return {
        "elf_class": "ELFCLASS32",
        "elf_data": "ELFDATA2MSB",
        "e_machine": "EM_PPC",
        "e_type": "ET_REL",
        "sections": [s.name for s in parsed.sections if s.name],
        "reloc_types_seen": [relocmod.reloc_name(t) for t in reloc_types],
        "disasm_head": "\n".join(disasm.splitlines()[:12]),
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Prove a PPC32 BE toolchain.")
    ap.add_argument("--toolchain", required=True,
                    help="Toolchain root (dir containing bin/clang or clang).")
    ap.add_argument("--linker", default=None,
                    help="Explicit ld.lld path if not beside clang.")
    ap.add_argument("--output", default=None,
                    help="Where to write the toolchain receipt JSON.")
    args = ap.parse_args(argv)

    try:
        tc = Toolchain.discover(args.toolchain, args.linker)
    except ToolchainError as e:
        print(f"doctor: FAILED to discover toolchain: {e}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as td:
        try:
            probes = _probe(tc, Path(td))
        except (ToolchainError, elfmod.ElfError) as e:
            print(f"doctor: toolchain probe FAILED: {e}", file=sys.stderr)
            return 3

    receipt = ToolchainReceipt(
        root=str(tc.root), triple=TARGET_TRIPLE, common_flags=COMMON_FLAGS,
        c_flags=C_FLAGS, cxx_flags=CXX_FLAGS,
        tools=tc.tool_versions(), probes=probes)

    print("doctor: PPC32 big-endian toolchain OK")
    print(f"  clang  : {receipt.tools['clang']['version']}")
    print(f"  linker : {receipt.tools['linker']['version']}")
    print(f"  relocs : {', '.join(probes['reloc_types_seen'])}")
    if args.output:
        receipt.write(Path(args.output))
        print(f"  receipt: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
