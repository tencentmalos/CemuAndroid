"""PPC32 relocation engine (spec section 5.2 whitelist).

This is the single source of truth for how the eight allowed relocation types
fix up module bytes. The Cemu C++ loader reimplements the same math; both are
exercised against the same golden fixtures (two distinct allocation bases) so a
divergence surfaces as a test failure rather than a wrong branch at runtime.

Definitions (spec 5.2):
  S = resolved guest symbol VA
  A = explicit signed addend (never re-read from already-relocated bytes)
  P = guest VA of the relocation site

Only the whitelisted types are implemented. Anything else -- including
REL14, GOT/PLT, SDA and TLS -- is rejected in prepare(), never silently zeroed.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Dict


# R_PPC_* type numbers (LLVM/binutils PPC32).
R_PPC_NONE = 0
R_PPC_ADDR32 = 1
R_PPC_ADDR16_LO = 4
R_PPC_ADDR16_HI = 5
R_PPC_ADDR16_HA = 6
R_PPC_REL24 = 10
R_PPC_REL32 = 26

# Rejected-but-named, so error messages are specific rather than "unknown".
R_PPC_ADDR14 = 7
R_PPC_REL14 = 11
R_PPC_GOT16 = 14
R_PPC_PLTREL24 = 18
R_PPC_SDAREL16 = 32
R_PPC_EMB_SDA21 = 109

RELOC_NAMES = {
    R_PPC_NONE: "R_PPC_NONE",
    R_PPC_ADDR32: "R_PPC_ADDR32",
    R_PPC_ADDR16_LO: "R_PPC_ADDR16_LO",
    R_PPC_ADDR16_HI: "R_PPC_ADDR16_HI",
    R_PPC_ADDR16_HA: "R_PPC_ADDR16_HA",
    R_PPC_REL24: "R_PPC_REL24",
    R_PPC_REL32: "R_PPC_REL32",
    R_PPC_ADDR14: "R_PPC_ADDR14",
    R_PPC_REL14: "R_PPC_REL14",
    R_PPC_GOT16: "R_PPC_GOT16",
    R_PPC_PLTREL24: "R_PPC_PLTREL24",
    R_PPC_SDAREL16: "R_PPC_SDAREL16",
    R_PPC_EMB_SDA21: "R_PPC_EMB_SDA21",
}

WHITELIST = frozenset({
    R_PPC_ADDR32,
    R_PPC_ADDR16_LO,
    R_PPC_ADDR16_HI,
    R_PPC_ADDR16_HA,
    R_PPC_REL24,
    R_PPC_REL32,
})

# REL24 signed displacement range: 26-bit signed, 4-byte aligned.
REL24_MIN = -0x02000000
REL24_MAX = 0x01FFFFFC

# Guest address space is 32-bit.
ADDR_MASK = 0xFFFFFFFF


class RelocationError(Exception):
    """Raised for any relocation that cannot be applied losslessly."""


def reloc_name(r_type: int) -> str:
    return RELOC_NAMES.get(r_type, f"R_PPC_{r_type}")


def is_whitelisted(r_type: int) -> bool:
    return r_type in WHITELIST


def _to_u32(value: int) -> int:
    return value & ADDR_MASK


def _to_s32(value: int) -> int:
    value &= ADDR_MASK
    return value - 0x100000000 if value & 0x80000000 else value


@dataclass
class RelocSite:
    """A single relocation resolved to concrete numbers, ready to apply."""
    r_type: int
    site_va: int          # P: guest VA of the patched bytes
    section_offset: int   # offset of the patched bytes within its section image
    symbol_va: int        # S: resolved target VA
    addend: int           # A: explicit signed addend
    symbol_name: str = ""


def _put_be16(buf: bytearray, off: int, value: int) -> None:
    if off < 0 or off + 2 > len(buf):
        raise RelocationError(f"halfword write at {off} out of section bounds")
    buf[off] = (value >> 8) & 0xFF
    buf[off + 1] = value & 0xFF


def _put_be32(buf: bytearray, off: int, value: int) -> None:
    if off < 0 or off + 4 > len(buf):
        raise RelocationError(f"word write at {off} out of section bounds")
    buf[off] = (value >> 24) & 0xFF
    buf[off + 1] = (value >> 16) & 0xFF
    buf[off + 2] = (value >> 8) & 0xFF
    buf[off + 3] = value & 0xFF


def _get_be32(buf: bytes, off: int) -> int:
    if off < 0 or off + 4 > len(buf):
        raise RelocationError(f"word read at {off} out of section bounds")
    return (buf[off] << 24) | (buf[off + 1] << 16) | (buf[off + 2] << 8) | buf[off + 3]


def _apply_addr32(buf: bytearray, site: RelocSite) -> None:
    value = _to_u32(site.symbol_va + site.addend)
    _put_be32(buf, site.section_offset, value)


def _apply_addr16_lo(buf: bytearray, site: RelocSite) -> None:
    value = (site.symbol_va + site.addend) & 0xFFFF
    # ADDR16 offset points at the halfword immediate field, typically insn+2.
    _put_be16(buf, site.section_offset, value)


def _apply_addr16_hi(buf: bytearray, site: RelocSite) -> None:
    value = ((site.symbol_va + site.addend) >> 16) & 0xFFFF
    _put_be16(buf, site.section_offset, value)


def _apply_addr16_ha(buf: bytearray, site: RelocSite) -> None:
    # Adjusted high: compensates for the low half being treated as signed.
    value = ((site.symbol_va + site.addend + 0x8000) >> 16) & 0xFFFF
    _put_be16(buf, site.section_offset, value)


def _apply_rel24(buf: bytearray, site: RelocSite) -> None:
    # D = S + A - P, 4-byte aligned, 26-bit signed range.
    d = _to_s32(site.symbol_va + site.addend - site.site_va)
    if d % 4 != 0:
        raise RelocationError(
            f"REL24 displacement {d} at {site.symbol_name or hex(site.site_va)} "
            "is not 4-byte aligned")
    if d < REL24_MIN or d > REL24_MAX:
        raise RelocationError(
            f"REL24 displacement {d} at {site.symbol_name or hex(site.site_va)} "
            f"out of range [{REL24_MIN}, {REL24_MAX}]")
    insn = _get_be32(buf, site.section_offset)
    primary = (insn >> 26) & 0x3F
    # Only I-form (18, b/bl/ba/bla) is a valid REL24 host instruction. B-form
    # conditional branches (16) are REL14 and are not in the whitelist.
    if primary != 18:
        raise RelocationError(
            f"REL24 target instruction 0x{insn:08x} is not an I-form branch "
            f"(primary opcode {primary}, expected 18)")
    # Preserve opcode + AA + LK, replace the 24-bit LI field only.
    li = (d >> 2) & 0x00FFFFFF
    new_insn = (insn & 0xFC000003) | (li << 2)
    _put_be32(buf, site.section_offset, new_insn)


def _apply_rel32(buf: bytearray, site: RelocSite) -> None:
    d = site.symbol_va + site.addend - site.site_va
    if d < -0x80000000 or d > 0x7FFFFFFF:
        raise RelocationError(
            f"REL32 displacement {d} not representable in 32 bits")
    _put_be32(buf, site.section_offset, _to_u32(d))


_HANDLERS: Dict[int, Callable[[bytearray, RelocSite], None]] = {
    R_PPC_ADDR32: _apply_addr32,
    R_PPC_ADDR16_LO: _apply_addr16_lo,
    R_PPC_ADDR16_HI: _apply_addr16_hi,
    R_PPC_ADDR16_HA: _apply_addr16_ha,
    R_PPC_REL24: _apply_rel24,
    R_PPC_REL32: _apply_rel32,
}


def apply_relocation(section_buf: bytearray, site: RelocSite) -> None:
    """Apply one whitelisted relocation to a mutable section image.

    Raises RelocationError for any non-whitelisted type or any value that
    cannot be represented losslessly. Never falls back to a zero fixup.
    """
    handler = _HANDLERS.get(site.r_type)
    if handler is None:
        raise RelocationError(
            f"relocation type {reloc_name(site.r_type)} is not in the v1 "
            "whitelist")
    handler(section_buf, site)
