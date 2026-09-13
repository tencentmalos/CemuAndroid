"""Golden tests for the PPC32 relocation engine (spec 5.2).

Each case is hand-computed from the spec formula and checked byte-for-byte.
The critical G0 acceptance property is exercised explicitly: applying the same
module at two different allocation bases must produce the base-appropriate
bytes -- REL24/REL32 track the displacement, ADDR32/ADDR16* track the absolute
symbol, and none of them silently zero.
"""

import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from gfbuild import reloc


def be32(v):
    v &= 0xFFFFFFFF
    return bytes(((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))


def be16(v):
    v &= 0xFFFF
    return bytes(((v >> 8) & 0xFF, v & 0xFF))


class Addr32Test(unittest.TestCase):
    def test_addr32_plain(self):
        buf = bytearray(4)
        site = reloc.RelocSite(reloc.R_PPC_ADDR32, 0, 0, 0x10203040, 0)
        reloc.apply_relocation(buf, site)
        self.assertEqual(bytes(buf), be32(0x10203040))

    def test_addr32_with_addend(self):
        buf = bytearray(4)
        site = reloc.RelocSite(reloc.R_PPC_ADDR32, 0, 0, 0x10203040, 0x8)
        reloc.apply_relocation(buf, site)
        self.assertEqual(bytes(buf), be32(0x10203048))

    def test_addr32_negative_addend_wraps_u32(self):
        buf = bytearray(4)
        site = reloc.RelocSite(reloc.R_PPC_ADDR32, 0, 0, 0x00000004, -8)
        reloc.apply_relocation(buf, site)
        self.assertEqual(bytes(buf), be32(0xFFFFFFFC))


class Addr16Test(unittest.TestCase):
    def test_addr16_lo(self):
        buf = bytearray(2)
        site = reloc.RelocSite(reloc.R_PPC_ADDR16_LO, 0, 0, 0x1234ABCD, 0)
        reloc.apply_relocation(buf, site)
        self.assertEqual(bytes(buf), be16(0xABCD))

    def test_addr16_hi(self):
        buf = bytearray(2)
        site = reloc.RelocSite(reloc.R_PPC_ADDR16_HI, 0, 0, 0x1234ABCD, 0)
        reloc.apply_relocation(buf, site)
        self.assertEqual(bytes(buf), be16(0x1234))

    def test_addr16_ha_no_carry(self):
        # low half < 0x8000 -> HA == HI
        buf = bytearray(2)
        site = reloc.RelocSite(reloc.R_PPC_ADDR16_HA, 0, 0, 0x12347FFF, 0)
        reloc.apply_relocation(buf, site)
        self.assertEqual(bytes(buf), be16(0x1234))

    def test_addr16_ha_with_carry(self):
        # low half >= 0x8000 -> HA == HI + 1 (the classic lis/addi pairing)
        buf = bytearray(2)
        site = reloc.RelocSite(reloc.R_PPC_ADDR16_HA, 0, 0, 0x12348000, 0)
        reloc.apply_relocation(buf, site)
        self.assertEqual(bytes(buf), be16(0x1235))

    def test_addr16_ha_carry_rollover(self):
        buf = bytearray(2)
        site = reloc.RelocSite(reloc.R_PPC_ADDR16_HA, 0, 0, 0xFFFF8000, 0)
        reloc.apply_relocation(buf, site)
        # (0xFFFF8000 + 0x8000) >> 16 = 0x10000 & 0xFFFF = 0
        self.assertEqual(bytes(buf), be16(0x0000))

    def test_addr16_lo_offset_points_at_immediate(self):
        # Simulate an `addi rD,rA,LO` where the immediate field is at insn+2.
        # opcode addi = 14; rD=3 rA=4 -> 0x38640000, immediate at offset 2.
        buf = bytearray(be32(0x38640000))
        site = reloc.RelocSite(reloc.R_PPC_ADDR16_LO, 0, 2, 0x0000BEEF, 0)
        reloc.apply_relocation(buf, site)
        self.assertEqual(bytes(buf), be32(0x3864BEEF))


class Rel24Test(unittest.TestCase):
    def _bl(self, aa=0, lk=1):
        # I-form branch template, LI=0.
        return (18 << 26) | (0 << 2) | (aa << 1) | lk

    def test_rel24_forward(self):
        insn = self._bl()
        buf = bytearray(be32(insn))
        # P=0x1000, S=0x1100 -> D=0x100
        site = reloc.RelocSite(reloc.R_PPC_REL24, 0x1000, 0, 0x1100, 0)
        reloc.apply_relocation(buf, site)
        expected = (18 << 26) | ((0x100 >> 2) << 2) | (0 << 1) | 1
        self.assertEqual(bytes(buf), be32(expected))

    def test_rel24_backward_preserves_lk(self):
        insn = self._bl(aa=0, lk=1)
        buf = bytearray(be32(insn))
        # P=0x2000, S=0x1000 -> D=-0x1000
        site = reloc.RelocSite(reloc.R_PPC_REL24, 0x2000, 0, 0x1000, 0)
        reloc.apply_relocation(buf, site)
        got = int.from_bytes(bytes(buf), "big")
        self.assertEqual(got & 1, 1, "LK must be preserved")
        self.assertEqual((got >> 26) & 0x3F, 18)
        # decode LI back
        li = (got >> 2) & 0xFFFFFF
        if li & 0x800000:
            li -= 0x1000000
        self.assertEqual(li << 2, -0x1000)

    def test_rel24_out_of_range(self):
        insn = self._bl()
        buf = bytearray(be32(insn))
        site = reloc.RelocSite(reloc.R_PPC_REL24, 0, 0, 0x08000000, 0)
        with self.assertRaises(reloc.RelocationError):
            reloc.apply_relocation(buf, site)

    def test_rel24_unaligned_rejected(self):
        insn = self._bl()
        buf = bytearray(be32(insn))
        site = reloc.RelocSite(reloc.R_PPC_REL24, 0x1000, 0, 0x1102, 0)
        with self.assertRaises(reloc.RelocationError):
            reloc.apply_relocation(buf, site)

    def test_rel24_rejects_non_iform_host(self):
        # B-form conditional branch (primary 16) is not a valid REL24 host.
        insn = (16 << 26)
        buf = bytearray(be32(insn))
        site = reloc.RelocSite(reloc.R_PPC_REL24, 0x1000, 0, 0x1100, 0)
        with self.assertRaises(reloc.RelocationError):
            reloc.apply_relocation(buf, site)


class Rel32Test(unittest.TestCase):
    def test_rel32_forward(self):
        buf = bytearray(4)
        site = reloc.RelocSite(reloc.R_PPC_REL32, 0x1000, 0, 0x1234, 0)
        reloc.apply_relocation(buf, site)
        self.assertEqual(bytes(buf), be32(0x234))

    def test_rel32_backward(self):
        buf = bytearray(4)
        site = reloc.RelocSite(reloc.R_PPC_REL32, 0x2000, 0, 0x1000, 0)
        reloc.apply_relocation(buf, site)
        self.assertEqual(bytes(buf), be32((-0x1000) & 0xFFFFFFFF))


class WhitelistTest(unittest.TestCase):
    def test_rejects_non_whitelisted(self):
        for t in (reloc.R_PPC_REL14, reloc.R_PPC_GOT16, reloc.R_PPC_EMB_SDA21,
                  reloc.R_PPC_ADDR14):
            buf = bytearray(4)
            site = reloc.RelocSite(t, 0, 0, 0x1000, 0)
            with self.assertRaises(reloc.RelocationError):
                reloc.apply_relocation(buf, site)


class TwoBaseGoldenTest(unittest.TestCase):
    """G0 acceptance: same relocations, two allocation bases, correct bytes."""

    def _apply_module(self, module_base, symbol_off):
        """A tiny module: a bl to an internal symbol (REL24), an ADDR32 pointer
        to the same symbol, and an ADDR16_HA/LO pair. Returns the three
        patched words."""
        # site layout inside module image:
        #   +0x00: bl <sym>        (REL24)
        #   +0x04: .long <sym>     (ADDR32)
        #   +0x08: lis r3, HA      (ADDR16_HA at +0x0A)
        #   +0x0C: addi r3,r3,LO   (ADDR16_LO at +0x0E)
        sym_va = module_base + symbol_off
        # REL24
        bl = (18 << 26) | 1  # LK=1
        buf0 = bytearray(be32(bl))
        reloc.apply_relocation(buf0, reloc.RelocSite(
            reloc.R_PPC_REL24, module_base + 0x00, 0, sym_va, 0))
        # ADDR32
        buf1 = bytearray(4)
        reloc.apply_relocation(buf1, reloc.RelocSite(
            reloc.R_PPC_ADDR32, module_base + 0x04, 0, sym_va, 0))
        # ADDR16_HA (immediate at insn+2)
        lis = bytearray(be32((15 << 26) | (3 << 21)))  # lis r3,0
        reloc.apply_relocation(lis, reloc.RelocSite(
            reloc.R_PPC_ADDR16_HA, module_base + 0x08, 2, sym_va, 0))
        addi = bytearray(be32((14 << 26) | (3 << 21) | (3 << 16)))  # addi r3,r3,0
        reloc.apply_relocation(addi, reloc.RelocSite(
            reloc.R_PPC_ADDR16_LO, module_base + 0x0C, 2, sym_va, 0))
        return bytes(buf0), bytes(buf1), bytes(lis), bytes(addi)

    def test_two_bases(self):
        base_a = 0x01800000  # codecave start
        base_b = 0x01810000  # a different codecave allocation
        sym_off = 0x40

        bl_a, a32_a, lis_a, addi_a = self._apply_module(base_a, sym_off)
        bl_b, a32_b, lis_b, addi_b = self._apply_module(base_b, sym_off)

        # REL24: displacement is base-independent (sym - site both shift equally)
        self.assertEqual(bl_a, bl_b, "REL24 displacement should be identical")
        # decode the displacement and confirm it equals sym_off - 0x00
        got = int.from_bytes(bl_a, "big")
        li = (got >> 2) & 0xFFFFFF
        if li & 0x800000:
            li -= 0x1000000
        self.assertEqual(li << 2, sym_off)

        # ADDR32: absolute, so it must differ by exactly (base_b - base_a).
        va_a = int.from_bytes(a32_a, "big")
        va_b = int.from_bytes(a32_b, "big")
        self.assertEqual(va_a, base_a + sym_off)
        self.assertEqual(va_b, base_b + sym_off)
        self.assertEqual(va_b - va_a, base_b - base_a)

        # ADDR16 HA/LO must reconstruct the correct absolute symbol at each base.
        def reconstruct(lis_word, addi_word):
            ha = int.from_bytes(lis_word, "big") & 0xFFFF
            lo = int.from_bytes(addi_word, "big") & 0xFFFF
            # lis loads HA<<16; addi adds sign-extended LO
            lo_signed = lo - 0x10000 if lo & 0x8000 else lo
            return ((ha << 16) + lo_signed) & 0xFFFFFFFF

        self.assertEqual(reconstruct(lis_a, addi_a), base_a + sym_off)
        self.assertEqual(reconstruct(lis_b, addi_b), base_b + sym_off)


if __name__ == "__main__":
    unittest.main()
