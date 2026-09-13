"""Module layout, relocation lowering, and deterministic digest.

Turns a parsed PPC32 ``ET_REL`` object plus a build recipe into the normalized
runtime representation the loader consumes: an ordered section layout, a symbol
table, a lowered relocation list (with explicit signed addends), and a
content-addressed ``image.bin``.

Key invariants from the spec:
  * The image is base-independent. Section offsets are relative to the module
    base; the loader adds the codecave base it allocates. build.py never bakes a
    final VA into image.bin (spec 5.1).
  * Every unresolved symbol must be a declared import or a loader-reserved
    symbol. Weak-undefined is not silently zeroed (spec 5.1).
  * Only whitelisted relocation types survive; anything else is rejected here,
    before a manifest is ever written (spec 5.2).
  * The digest is computed over normalized, ordered semantic content and
    excludes non-semantic fields, so record ordering / debug paths cannot change
    the runtime layout (spec 5.1).
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from . import elf as elfmod
from . import reloc as relocmod


# Loader-reserved data symbol that receives the module context handle.
RESERVED_SYMBOLS = frozenset({"__cemu_custom_context"})

# Section kind ordering in the image (text first for branch-reach reasoning).
_KIND_ORDER = {"text": 0, "rodata": 1, "data": 2, "bss": 3}

_ALLOWED_ALIGN = (4, 8, 16, 256)


class BuildError(Exception):
    pass


def _kind_for_section(s: elfmod.ElfSection) -> Optional[str]:
    """Classify an allocatable section into a runtime kind, or None to drop."""
    if not s.is_alloc:
        return None
    if s.is_nobits:
        return "bss"
    if s.is_exec:
        return "text"
    if s.is_write:
        return "data"
    return "rodata"


@dataclass
class LaidOutSection:
    id: str
    kind: str
    elf_index: int
    image_offset: int
    file_size: int
    memory_size: int
    alignment: int
    data: bytes  # empty for bss


@dataclass
class ModuleLayout:
    sections: List[LaidOutSection] = field(default_factory=list)
    total_size: int = 0
    _by_elf_index: Dict[int, LaidOutSection] = field(default_factory=dict)

    def section_for_elf(self, idx: int) -> Optional[LaidOutSection]:
        return self._by_elf_index.get(idx)


def _norm_align(a: int) -> int:
    if a <= 1:
        return 4
    if a in _ALLOWED_ALIGN:
        return a
    raise BuildError(f"alignment {a} is not supported in v1 (allowed: {_ALLOWED_ALIGN})")


def _checked_add(a: int, b: int) -> int:
    r = a + b
    if r < 0 or r > 0xFFFFFFFF:
        raise BuildError(f"layout integer overflow ({a}+{b})")
    return r


def _align_up(value: int, align: int) -> int:
    return (value + align - 1) & ~(align - 1)


def lay_out_module(obj: elfmod.ElfObject, max_bytes: int) -> ModuleLayout:
    """Assign each allocatable section an image offset within the module."""
    candidates = []
    for s in obj.sections:
        kind = _kind_for_section(s)
        if kind is None:
            continue
        if s.sh_size == 0 and kind != "bss":
            continue
        candidates.append((kind, s))

    if not candidates:
        raise BuildError("module has no allocatable sections")

    # Deterministic order: kind, then section name.
    candidates.sort(key=lambda ks: (_KIND_ORDER[ks[0]], ks[1].name))

    layout = ModuleLayout()
    cursor = 0
    for kind, s in candidates:
        align = _norm_align(s.sh_addralign)
        cursor = _align_up(cursor, align)
        file_size = 0 if kind == "bss" else s.sh_size
        memory_size = s.sh_size
        data = b"" if kind == "bss" else s.data
        if kind != "bss" and len(data) != file_size:
            raise BuildError(f"section '{s.name}' data/size mismatch")
        laid = LaidOutSection(
            id=s.name or f"sec{s.index}", kind=kind, elf_index=s.index,
            image_offset=cursor, file_size=file_size, memory_size=memory_size,
            alignment=align, data=data)
        layout.sections.append(laid)
        layout._by_elf_index[s.index] = laid
        cursor = _checked_add(cursor, memory_size)

    layout.total_size = cursor
    if layout.total_size > max_bytes:
        raise BuildError(
            f"module size {layout.total_size} exceeds max_module_bytes {max_bytes}")
    return layout


@dataclass
class ResolvedImport:
    symbol: str
    kind: str
    guest_va: Optional[int] = None
    rpl_module: Optional[str] = None
    rpl_symbol: Optional[str] = None
    sdk_export_id: Optional[int] = None
    original_bytes: Optional[str] = None
    abi_evidence: Optional[str] = None


def build_import_index(recipe_imports: List[dict]) -> Dict[str, ResolvedImport]:
    idx: Dict[str, ResolvedImport] = {}
    for imp in recipe_imports:
        ri = ResolvedImport(
            symbol=imp["symbol"], kind=imp["kind"],
            guest_va=imp.get("guest_va"), rpl_module=imp.get("rpl_module"),
            rpl_symbol=imp.get("rpl_symbol"), sdk_export_id=imp.get("sdk_export_id"),
            original_bytes=imp.get("original_bytes"),
            abi_evidence=imp.get("abi_evidence"))
        if ri.symbol in idx:
            raise BuildError(f"duplicate import declaration for '{ri.symbol}'")
        idx[ri.symbol] = ri
    return idx


def classify_symbols(obj: elfmod.ElfObject, layout: ModuleLayout,
                     imports: Dict[str, ResolvedImport]) -> List[dict]:
    """Produce the manifest symbol list and verify every undefined symbol is
    a declared import or reserved symbol."""
    out: List[dict] = []
    seen = set()
    for sym in obj.symbols:
        if not sym.name:
            continue
        if sym.sym_type in (elfmod.STT_FILE, elfmod.STT_SECTION):
            continue
        if sym.is_undefined:
            if sym.name in imports or sym.name in RESERVED_SYMBOLS:
                continue
            if sym.binding == elfmod.STB_WEAK:
                raise BuildError(
                    f"weak undefined symbol '{sym.name}' is not declared as an "
                    "import; refusing to zero it")
            raise BuildError(
                f"undefined symbol '{sym.name}' is neither a declared import "
                "nor a loader-reserved symbol")
        if not sym.is_defined:
            continue
        laid = layout.section_for_elf(sym.st_shndx)
        if laid is None:
            # Symbol defined in a non-allocated section (e.g. debug); skip.
            continue
        if sym.binding == elfmod.STB_LOCAL and sym.name in seen:
            # keep locals unique-by-name for the manifest, prefer first
            continue
        kind = {elfmod.STT_FUNC: "func", elfmod.STT_OBJECT: "object"}.get(
            sym.sym_type, "notype")
        if sym.name in seen:
            raise BuildError(f"duplicate defined symbol name '{sym.name}'")
        seen.add(sym.name)
        out.append({
            "name": sym.name,
            "section": laid.id,
            "offset": sym.st_value,  # value is section-relative in ET_REL
            "kind": kind,
        })
    out.sort(key=lambda d: (d["section"], d["offset"], d["name"]))
    return out


def lower_relocations(obj: elfmod.ElfObject, layout: ModuleLayout) -> List[dict]:
    """Translate ELF RELA entries into manifest relocations, rejecting any
    non-whitelisted type. Addends stay explicit and signed."""
    out: List[dict] = []
    for tgt_index, rels in obj.relocations.items():
        laid = layout.section_for_elf(tgt_index)
        if laid is None:
            # Relocation targeting a non-allocated section: only tolerable if it
            # is a debug/comment section, which we drop. Guard anyway.
            tgt = obj.sections[tgt_index]
            if tgt.is_alloc:
                raise BuildError(
                    f"relocation targets allocatable section '{tgt.name}' that "
                    "was not laid out")
            continue
        for r in rels:
            if not relocmod.is_whitelisted(r.r_type):
                raise BuildError(
                    f"relocation type {relocmod.reloc_name(r.r_type)} in section "
                    f"'{laid.id}' is not in the v1 whitelist")
            sym = obj.symbols[r.sym_index]
            sym_name = sym.name
            if not sym_name:
                # Local, section-relative symbol: name it by its section so the
                # loader can resolve it against an internal symbol.
                sec = obj.sections[sym.st_shndx] if sym.is_defined else None
                if sec is None:
                    raise BuildError("relocation against an unnamed non-local symbol")
                sym_name = f"@section:{sec.name}"
            if r.r_offset + 2 > laid.memory_size:
                raise BuildError(
                    f"relocation offset {r.r_offset} out of section '{laid.id}'")
            out.append({
                "section": laid.id,
                "offset": r.r_offset,
                "type": relocmod.reloc_name(r.r_type),
                "symbol": sym_name,
                "addend": r.r_addend,
            })
    out.sort(key=lambda d: (d["section"], d["offset"], d["type"], d["symbol"]))
    return out


def build_image(layout: ModuleLayout) -> bytes:
    """Concatenate non-bss section data into the base-independent image."""
    if not layout.sections:
        return b""
    buf = bytearray(layout.total_size)
    for s in layout.sections:
        if s.kind == "bss":
            continue
        buf[s.image_offset:s.image_offset + s.file_size] = s.data
    return bytes(buf)


def _canonical_json(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True).encode("utf-8")


def compute_module_digest(sections: List[dict], symbols: List[dict],
                          relocations: List[dict], hooks: List[dict],
                          imports: List[dict], image_sha256: str) -> str:
    """Deterministic digest over ordered semantic content plus the image SHA.

    The image SHA is folded in so the digest is a complete content fingerprint:
    a structural edit changes the JSON payload, and an image byte edit changes
    ``image_sha256``. Both are detectable (spec 11/G0). The image is
    pre-relocation and base-independent, so the digest stays stable across
    allocation bases and across non-semantic debug-path differences (spec 5.1).
    """
    def norm_sections(items):
        return sorted(({
            "id": s["id"], "kind": s["kind"], "image_offset": s["image_offset"],
            "file_size": s["file_size"], "memory_size": s["memory_size"],
            "alignment": s["alignment"],
        } for s in items), key=lambda s: s["image_offset"])

    payload = {
        "image_sha256": image_sha256,
        "sections": norm_sections(sections),
        "symbols": sorted(symbols, key=lambda d: (d["section"], d["offset"], d["name"])),
        "relocations": sorted(relocations, key=lambda d: (d["section"], d["offset"], d["type"], d["symbol"])),
        "hooks": sorted(hooks, key=lambda d: (d["guest_va"], d["kind"], d["target_symbol"])),
        "imports": sorted(imports, key=lambda d: d["symbol"]),
    }
    return hashlib.sha256(_canonical_json(payload)).hexdigest()
