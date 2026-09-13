"""Strict ELF32 big-endian reader for PPC relocatable objects.

Scope is intentionally narrow: only what the guest-function pipeline needs to
consume a partially-linked (``ld.lld -r``) PPC32 big-endian ``ET_REL`` object.
Every structural expectation from the spec (class/endian/machine/type, section
and symbol table integrity, RELA-with-addend) is checked explicitly and turned
into a :class:`ElfError` rather than a silent default. There is no tolerance
mode; the loader on the Cemu side is equally strict, so accepting something
here that the loader rejects would be a bug in the tool.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional


# ELF identification
ELFMAG = b"\x7fELF"
ELFCLASS32 = 1
ELFDATA2MSB = 2
EV_CURRENT = 1

# e_type
ET_REL = 1

# e_machine
EM_PPC = 20

# Special section indices
SHN_UNDEF = 0
SHN_ABS = 0xFFF1
SHN_COMMON = 0xFFF2
SHN_LORESERVE = 0xFF00

# sh_type
SHT_NULL = 0
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9

# sh_flags
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4

# Symbol binding / type (st_info)
STB_LOCAL = 0
STB_GLOBAL = 1
STB_WEAK = 2

STT_NOTYPE = 0
STT_OBJECT = 1
STT_FUNC = 2
STT_SECTION = 3
STT_FILE = 4


class ElfError(Exception):
    """Raised for any structural violation of the expected ELF shape."""


@dataclass
class ElfSection:
    index: int
    name: str
    sh_name: int
    sh_type: int
    sh_flags: int
    sh_addr: int
    sh_offset: int
    sh_size: int
    sh_link: int
    sh_info: int
    sh_addralign: int
    sh_entsize: int
    data: bytes = b""

    @property
    def is_alloc(self) -> bool:
        return bool(self.sh_flags & SHF_ALLOC)

    @property
    def is_write(self) -> bool:
        return bool(self.sh_flags & SHF_WRITE)

    @property
    def is_exec(self) -> bool:
        return bool(self.sh_flags & SHF_EXECINSTR)

    @property
    def is_nobits(self) -> bool:
        return self.sh_type == SHT_NOBITS


@dataclass
class ElfSymbol:
    index: int
    name: str
    st_name: int
    st_value: int
    st_size: int
    st_info: int
    st_other: int
    st_shndx: int

    @property
    def binding(self) -> int:
        return self.st_info >> 4

    @property
    def sym_type(self) -> int:
        return self.st_info & 0xF

    @property
    def is_undefined(self) -> bool:
        return self.st_shndx == SHN_UNDEF

    @property
    def is_defined(self) -> bool:
        return self.st_shndx != SHN_UNDEF and self.st_shndx < SHN_LORESERVE


@dataclass
class ElfRelocation:
    # Section this relocation is applied to (the RELA's sh_info target).
    target_section_index: int
    r_offset: int
    r_type: int
    sym_index: int
    r_addend: int  # signed


@dataclass
class ElfObject:
    e_type: int
    e_machine: int
    sections: List[ElfSection] = field(default_factory=list)
    symbols: List[ElfSymbol] = field(default_factory=list)
    # relocations keyed by target section index
    relocations: Dict[int, List[ElfRelocation]] = field(default_factory=dict)
    _symtab_index: int = -1

    def section_by_name(self, name: str) -> Optional[ElfSection]:
        for s in self.sections:
            if s.name == name:
                return s
        return None


def _u16(b: bytes, off: int) -> int:
    return struct.unpack_from(">H", b, off)[0]


def _u32(b: bytes, off: int) -> int:
    return struct.unpack_from(">I", b, off)[0]


def _s32(b: bytes, off: int) -> int:
    return struct.unpack_from(">i", b, off)[0]


def _cstr(table: bytes, off: int) -> str:
    if off >= len(table):
        raise ElfError(f"string offset {off} out of range ({len(table)})")
    end = table.find(b"\x00", off)
    if end < 0:
        raise ElfError("unterminated string in string table")
    return table[off:end].decode("utf-8", "strict")


def parse_elf(blob: bytes) -> ElfObject:
    """Parse a PPC32 big-endian ET_REL object with strict validation."""
    if len(blob) < 52:
        raise ElfError("file too small for an ELF32 header")
    if blob[:4] != ELFMAG:
        raise ElfError("bad ELF magic")
    ei_class = blob[4]
    ei_data = blob[5]
    ei_version = blob[6]
    if ei_class != ELFCLASS32:
        raise ElfError(f"expected ELFCLASS32, got {ei_class}")
    if ei_data != ELFDATA2MSB:
        raise ElfError(f"expected big-endian (ELFDATA2MSB), got {ei_data}")
    if ei_version != EV_CURRENT:
        raise ElfError(f"unexpected EI_VERSION {ei_version}")

    e_type = _u16(blob, 16)
    e_machine = _u16(blob, 18)
    e_version = _u32(blob, 20)
    e_shoff = _u32(blob, 32)
    e_ehsize = _u16(blob, 40)
    e_shentsize = _u16(blob, 46)
    e_shnum = _u16(blob, 48)
    e_shstrndx = _u16(blob, 50)

    if e_type != ET_REL:
        raise ElfError(f"expected ET_REL, got e_type={e_type}")
    if e_machine != EM_PPC:
        raise ElfError(f"expected EM_PPC (20), got e_machine={e_machine}")
    if e_version != EV_CURRENT:
        raise ElfError(f"unexpected e_version {e_version}")
    if e_ehsize < 52:
        raise ElfError(f"implausible e_ehsize {e_ehsize}")
    if e_shentsize != 40:
        raise ElfError(f"expected 40-byte section headers, got {e_shentsize}")
    if e_shoff == 0 or e_shnum == 0:
        raise ElfError("object has no section headers")
    if e_shoff + e_shnum * e_shentsize > len(blob):
        raise ElfError("section header table out of range")
    if e_shstrndx >= e_shnum:
        raise ElfError("e_shstrndx out of range")

    # First pass: raw section headers.
    raw = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
         sh_link, sh_info, sh_addralign, sh_entsize) = struct.unpack_from(
            ">IIIIIIIIII", blob, base)
        raw.append(dict(sh_name=sh_name, sh_type=sh_type, sh_flags=sh_flags,
                        sh_addr=sh_addr, sh_offset=sh_offset, sh_size=sh_size,
                        sh_link=sh_link, sh_info=sh_info,
                        sh_addralign=sh_addralign, sh_entsize=sh_entsize))

    # Section header string table.
    shstr = raw[e_shstrndx]
    if shstr["sh_type"] != SHT_STRTAB:
        raise ElfError("e_shstrndx does not point at a string table")
    if shstr["sh_offset"] + shstr["sh_size"] > len(blob):
        raise ElfError("shstrtab out of range")
    shstrtab = blob[shstr["sh_offset"]:shstr["sh_offset"] + shstr["sh_size"]]

    obj = ElfObject(e_type=e_type, e_machine=e_machine)
    for i, r in enumerate(raw):
        name = _cstr(shstrtab, r["sh_name"]) if r["sh_type"] != SHT_NULL else ""
        data = b""
        if r["sh_type"] != SHT_NOBITS and r["sh_type"] != SHT_NULL and r["sh_size"] > 0:
            if r["sh_offset"] + r["sh_size"] > len(blob):
                raise ElfError(f"section '{name}' data out of range")
            data = blob[r["sh_offset"]:r["sh_offset"] + r["sh_size"]]
        obj.sections.append(ElfSection(
            index=i, name=name, sh_name=r["sh_name"], sh_type=r["sh_type"],
            sh_flags=r["sh_flags"], sh_addr=r["sh_addr"], sh_offset=r["sh_offset"],
            sh_size=r["sh_size"], sh_link=r["sh_link"], sh_info=r["sh_info"],
            sh_addralign=r["sh_addralign"], sh_entsize=r["sh_entsize"], data=data))

    _parse_symbols(obj, blob)
    _parse_relocations(obj, blob)
    return obj


def _parse_symbols(obj: ElfObject, blob: bytes) -> None:
    symtab_idx = -1
    for s in obj.sections:
        if s.sh_type == SHT_SYMTAB:
            if symtab_idx != -1:
                raise ElfError("multiple SYMTAB sections are not supported")
            symtab_idx = s.index
    if symtab_idx == -1:
        raise ElfError("no symbol table found")
    symtab = obj.sections[symtab_idx]
    if symtab.sh_entsize != 16:
        raise ElfError(f"expected 16-byte symbols, got {symtab.sh_entsize}")
    if symtab.sh_size % 16 != 0:
        raise ElfError("symbol table size is not a multiple of 16")
    if symtab.sh_link >= len(obj.sections):
        raise ElfError("symtab sh_link out of range")
    strtab_sec = obj.sections[symtab.sh_link]
    if strtab_sec.sh_type != SHT_STRTAB:
        raise ElfError("symtab sh_link does not point at a string table")
    strtab = strtab_sec.data

    count = symtab.sh_size // 16
    for i in range(count):
        base = i * 16
        st_name = _u32(symtab.data, base)
        st_value = _u32(symtab.data, base + 4)
        st_size = _u32(symtab.data, base + 8)
        st_info = symtab.data[base + 12]
        st_other = symtab.data[base + 13]
        st_shndx = _u16(symtab.data, base + 14)
        name = _cstr(strtab, st_name) if st_name != 0 else ""
        obj.symbols.append(ElfSymbol(
            index=i, name=name, st_name=st_name, st_value=st_value,
            st_size=st_size, st_info=st_info, st_other=st_other,
            st_shndx=st_shndx))
    obj._symtab_index = symtab_idx


def _parse_relocations(obj: ElfObject, blob: bytes) -> None:
    for s in obj.sections:
        if s.sh_type == SHT_REL:
            raise ElfError(
                f"section '{s.name}' uses SHT_REL; only RELA (explicit addend) "
                "is accepted")
        if s.sh_type != SHT_RELA:
            continue
        if s.sh_entsize != 12:
            raise ElfError(f"expected 12-byte RELA entries in '{s.name}'")
        if s.sh_size % 12 != 0:
            raise ElfError(f"RELA section '{s.name}' size not a multiple of 12")
        if s.sh_link != obj._symtab_index:
            raise ElfError(f"RELA '{s.name}' sh_link does not match the symtab")
        target = s.sh_info
        if target >= len(obj.sections):
            raise ElfError(f"RELA '{s.name}' target section out of range")
        rels: List[ElfRelocation] = []
        count = s.sh_size // 12
        for i in range(count):
            base = i * 12
            r_offset = _u32(s.data, base)
            r_info = _u32(s.data, base + 4)
            r_addend = _s32(s.data, base + 8)
            sym_index = r_info >> 8
            r_type = r_info & 0xFF
            if sym_index >= len(obj.symbols):
                raise ElfError(f"relocation references symbol {sym_index} "
                               "out of range")
            rels.append(ElfRelocation(
                target_section_index=target, r_offset=r_offset, r_type=r_type,
                sym_index=sym_index, r_addend=r_addend))
        obj.relocations.setdefault(target, []).extend(rels)
