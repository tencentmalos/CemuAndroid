"""Hook instruction synthesis and original-trampoline generation (spec 5.3).

Given a hook's guest VA, its resolved target VA, and the original bytes, this
produces the exact big-endian PPC instruction(s) to install, plus the original
trampoline body for entry hooks. Every jump is validated: opcode form, AA/LK
bits, alignment, and reachability. Nothing here silently falls back to a lossy
jump -- an unreachable or unsupported case raises so the builder rejects it.

Branch encodings used:
  I-form (primary opcode 18): b/ba/bl/bla
    bits: [6:opcode=18][24:LI][1:AA][1:LK]
    - b   : AA=0 LK=0 (PC-relative, no link)
    - ba  : AA=1 LK=0 (absolute, no link)
    - bl  : AA=0 LK=1 (PC-relative, link -> sets LR)
    - bla : AA=1 LK=1 (absolute, link)

The codecave region (0x01800000..0x01BFFFFF) is inside the low 32 MiB that a
PPC absolute branch can encode, so a single ba/bla into the module is valid.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional


CODECAVE_BASE = 0x01800000
CODECAVE_END = 0x01C00000  # exclusive
# Absolute branch LI field is 26-bit signed; low 32 MiB is encodable as AA=1.
ABS_BRANCH_MAX = 0x01FFFFFC

REL24_MIN = -0x02000000
REL24_MAX = 0x01FFFFFC


class HookError(Exception):
    pass


def _be32(value: int) -> bytes:
    value &= 0xFFFFFFFF
    return bytes(((value >> 24) & 0xFF, (value >> 16) & 0xFF,
                  (value >> 8) & 0xFF, value & 0xFF))


def _read_be32(b: bytes, off: int = 0) -> int:
    return (b[off] << 24) | (b[off + 1] << 16) | (b[off + 2] << 8) | b[off + 3]


def encode_branch(site_va: int, target_va: int, *, link: bool,
                  prefer_absolute: bool = True) -> bytes:
    """Encode a single b/ba/bl/bla from site_va to target_va.

    Prefers PC-relative (REL24) when in range; uses absolute (AA=1) only when
    the target is within the low 32 MiB. Raises if neither is possible.
    """
    if site_va % 4 != 0 or target_va % 4 != 0:
        raise HookError("branch site and target must be 4-byte aligned")
    lk = 1 if link else 0

    disp = target_va - site_va
    can_rel = REL24_MIN <= disp <= REL24_MAX and disp % 4 == 0
    can_abs = 0 <= target_va <= ABS_BRANCH_MAX

    use_abs = prefer_absolute and can_abs and not (0 <= target_va < 0x100)
    # Prefer relative when both are available and the module is reachable; this
    # keeps r12/CTR untouched and avoids depending on absolute placement. But
    # for codecave targets absolute is equally safe and independent of site.
    if can_rel:
        li = (disp >> 2) & 0x00FFFFFF
        insn = (18 << 26) | (li << 2) | (0 << 1) | lk  # AA=0
        return _be32(insn)
    if can_abs:
        li = (target_va >> 2) & 0x00FFFFFF
        insn = (18 << 26) | (li << 2) | (1 << 1) | lk  # AA=1
        return _be32(insn)
    raise HookError(
        f"target 0x{target_va:08x} not reachable from 0x{site_va:08x} with a "
        "single branch (needs an explicit veneer/bridge)")


def validate_callsite_original(original: bytes) -> dict:
    """A callsite hook may only overwrite a bl/bla. Returns its decoded form."""
    if len(original) != 4:
        raise HookError("callsite hook must cover exactly one 4-byte instruction")
    insn = _read_be32(original)
    primary = (insn >> 26) & 0x3F
    if primary != 18:
        raise HookError(
            f"callsite original 0x{insn:08x} is not an I-form branch "
            f"(primary opcode {primary})")
    aa = (insn >> 1) & 1
    lk = insn & 1
    if lk != 1:
        raise HookError("callsite original must be a linking branch (bl/bla)")
    return {"aa": aa, "lk": lk, "insn": insn}


# Entry-hook prologue whitelist for automatic original replay (spec 5.3).
# Only these first instructions can be safely relocated into the trampoline.
def _is_mflr_r0(insn: int) -> bool:
    # mfspr r0, LR : 0x7C0802A6
    return insn == 0x7C0802A6


def _is_stwu_r1(insn: int) -> bool:
    # stwu r1, -N(r1): primary 37, rS=1, rA=1
    return ((insn >> 26) & 0x3F) == 37 and ((insn >> 21) & 0x1F) == 1 and \
           ((insn >> 16) & 0x1F) == 1


def _is_mr_or_stw_savearea(insn: int) -> bool:
    # stw rS, D(r1): primary 36, rA=1  (audited stack save)
    if ((insn >> 26) & 0x3F) == 36 and ((insn >> 16) & 0x1F) == 1:
        return True
    return False


_PROLOGUE_PREDICATES = (_is_mflr_r0, _is_stwu_r1, _is_mr_or_stw_savearea)


@dataclass
class EntryTrampoline:
    replayed_bytes: bytes      # relocated original prologue instructions
    resume_branch: bytes       # branch back to (guest_va + len(replayed))
    resume_va_offset: int      # bytes of original consumed


def _relocatable_prologue_instr(insn: int) -> bool:
    return any(p(insn) for p in _PROLOGUE_PREDICATES)


def build_entry_trampoline(guest_va: int, original: bytes,
                           trampoline_va: int) -> EntryTrampoline:
    """Build an original trampoline for an entry hook.

    Replays a whitelisted prologue then branches back to the instruction after
    the overwritten region. All replayed instructions must be
    position-independent (the whitelist guarantees this); a b/ba would need
    re-encoding and is not allowed in the replayed window.
    """
    if len(original) < 4 or len(original) % 4 != 0:
        raise HookError("entry hook original must be a multiple of 4 bytes")
    n = len(original) // 4
    for i in range(n):
        insn = _read_be32(original, i * 4)
        primary = (insn >> 26) & 0x3F
        if primary == 18 or primary == 16:
            raise HookError(
                "entry prologue contains a branch; requires a manual bridge")
        if not _relocatable_prologue_instr(insn):
            raise HookError(
                f"entry prologue instruction 0x{insn:08x} is not in the "
                "auto-replay whitelist; requires an explicit .S bridge")
    resume_va = guest_va + len(original)
    branch_site = trampoline_va + len(original)
    resume_branch = encode_branch(branch_site, resume_va, link=False)
    return EntryTrampoline(replayed_bytes=bytes(original),
                           resume_branch=resume_branch,
                           resume_va_offset=len(original))


def install_branch_for_hook(kind: str, guest_va: int, target_va: int,
                            original: bytes) -> bytes:
    """Return the bytes to write at guest_va for a callsite/entry hook."""
    if kind == "callsite":
        validate_callsite_original(original)
        # Preserve link semantics: a callsite is a bl/bla, so we keep LK=1.
        return encode_branch(guest_va, target_va, link=True)
    if kind == "entry":
        # Entry uses a non-linking branch so it does not clobber LR.
        return encode_branch(guest_va, target_va, link=False)
    if kind == "manual":
        raise HookError("manual hooks provide their own bridge; no auto branch")
    raise HookError(f"unknown hook kind '{kind}'")
