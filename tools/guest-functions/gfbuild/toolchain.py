"""Toolchain discovery, invocation and receipt.

doctor.py proves a real PPC32 big-endian toolchain exists by compiling and
linking a fixture and inspecting the output; build.py replays the same flags.
The flags here are the spec 5.1 starting candidates, fixed by doctor's actual
probe rather than assumed. This module deliberately never falls back to a
PATH compiler: the caller passes an explicit toolchain root, and every tool is
resolved under it.
"""

from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional


# Spec 5.1 baseline flags. doctor.py records the exact set that passed.
TARGET_TRIPLE = "powerpc-unknown-eabi"
COMMON_FLAGS = [
    f"--target={TARGET_TRIPLE}",
    "-mcpu=750",
    "-m32",
    "-mbig-endian",
    "-ffreestanding",
    "-fno-pic",
    "-fno-pie",
    "-fno-common",
    "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables",
    "-fno-stack-protector",
    "-fno-builtin",
    "-ffp-contract=off",
    "-mno-altivec",
    "-fno-tree-vectorize",
    "-fno-vectorize",
    "-fno-slp-vectorize",
    "-O2",
]
C_FLAGS = ["-std=c11"]
CXX_FLAGS = ["-std=c++17", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics"]
ASM_FLAGS: List[str] = []


class ToolchainError(Exception):
    pass


@dataclass
class Toolchain:
    root: Path
    clang: Path
    linker: Path
    readelf: Path
    objdump: Path

    @staticmethod
    def discover(root: str | Path, linker_override: Optional[str] = None) -> "Toolchain":
        rootp = Path(root)
        bindir = rootp / "bin" if (rootp / "bin").is_dir() else rootp
        clang = bindir / "clang"
        if not clang.exists():
            raise ToolchainError(f"clang not found under {bindir}")
        # lld may live beside clang or be provided explicitly (Homebrew ships it
        # as a separate formula; the NDK ships a usable arch-agnostic ld.lld).
        linker: Optional[Path] = None
        if linker_override:
            linker = Path(linker_override)
            if not linker.exists():
                raise ToolchainError(f"linker override {linker} not found")
        else:
            for cand in (bindir / "ld.lld", bindir / "lld"):
                if cand.exists():
                    linker = cand
                    break
        if linker is None:
            raise ToolchainError(
                f"no ld.lld found under {bindir}; pass --linker explicitly")
        readelf = bindir / "llvm-readelf"
        objdump = bindir / "llvm-objdump"
        for t in (readelf, objdump):
            if not t.exists():
                raise ToolchainError(f"missing {t.name} under {bindir}")
        return Toolchain(root=rootp, clang=clang, linker=linker,
                         readelf=readelf, objdump=objdump)

    def _run(self, args: List[str]) -> subprocess.CompletedProcess:
        return subprocess.run(args, capture_output=True, text=True)

    def compile_one(self, src: Path, out_o: Path, lang: str,
                    include_dirs: List[Path], extra: List[str]) -> None:
        flags = list(COMMON_FLAGS)
        if lang == "c":
            flags += C_FLAGS
        elif lang == "cpp":
            flags += CXX_FLAGS
        elif lang == "asm":
            flags += ASM_FLAGS
        else:
            raise ToolchainError(f"unknown source language '{lang}'")
        for inc in include_dirs:
            flags += ["-I", str(inc)]
        flags += extra
        # Emit dependency file for provenance (transitive includes).
        dep = out_o.with_suffix(out_o.suffix + ".d")
        args = [str(self.clang)] + flags + ["-MD", "-MF", str(dep),
                                            "-c", str(src), "-o", str(out_o)]
        cp = self._run(args)
        if cp.returncode != 0:
            raise ToolchainError(
                f"compile failed for {src.name}:\n{cp.stderr}\ncmd: {' '.join(args)}")

    def partial_link(self, objs: List[Path], out_o: Path) -> None:
        args = [str(self.linker), "-r"] + [str(o) for o in objs] + ["-o", str(out_o)]
        cp = self._run(args)
        if cp.returncode != 0:
            raise ToolchainError(f"partial link failed:\n{cp.stderr}")

    def tool_versions(self) -> dict:
        out = {}
        for name, path in (("clang", self.clang), ("linker", self.linker),
                           ("readelf", self.readelf)):
            cp = self._run([str(path), "--version"])
            first = cp.stdout.strip().splitlines()[0] if cp.stdout else ""
            out[name] = {"path": str(path), "version": first,
                         "sha256": _sha256_file(path)}
        return out


def _sha256_file(path: Path) -> str:
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


@dataclass
class ToolchainReceipt:
    root: str
    triple: str
    common_flags: List[str]
    c_flags: List[str]
    cxx_flags: List[str]
    tools: dict = field(default_factory=dict)
    probes: dict = field(default_factory=dict)

    def to_json(self) -> dict:
        return {
            "schema": "cemu.guest-toolchain-receipt.v1",
            "root": self.root,
            "triple": self.triple,
            "common_flags": self.common_flags,
            "c_flags": self.c_flags,
            "cxx_flags": self.cxx_flags,
            "tools": self.tools,
            "probes": self.probes,
        }

    def write(self, path: Path) -> None:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self.to_json(), f, indent=2, sort_keys=True)
            f.write("\n")
