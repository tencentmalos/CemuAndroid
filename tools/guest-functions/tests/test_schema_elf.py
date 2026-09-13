"""Schema validator and strict-ELF-reader rejection tests (spec 4.1, 12)."""

import struct
import sys
import unittest
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_ROOT))

from gfbuild import elf as elfmod       # noqa: E402
from gfbuild import schema as schemamod  # noqa: E402


class SchemaValidatorTest(unittest.TestCase):
    def setUp(self):
        self.build_schema = schemamod.load_schema(
            _ROOT / "schema" / "build.recipe.v1.schema.json")
        self.rt_schema = schemamod.load_schema(
            _ROOT / "schema" / "runtime.manifest.v1.schema.json")

    def test_valid_minimal_recipe(self):
        recipe = {
            "schema": "cemu.guest-function-build.v1",
            "module": "u-king",
            "identity_ref": "identity.json",
            "sources": [{"path": "a.c", "lang": "c"}],
        }
        self.assertEqual(schemamod.validate(recipe, self.build_schema), [])

    def test_recipe_missing_required(self):
        recipe = {"schema": "cemu.guest-function-build.v1", "module": "x"}
        errs = schemamod.validate(recipe, self.build_schema)
        self.assertTrue(any("sources" in e for e in errs))

    def test_recipe_bad_const(self):
        recipe = {
            "schema": "wrong.schema", "module": "x",
            "identity_ref": "i.json", "sources": [{"path": "a.c", "lang": "c"}],
        }
        errs = schemamod.validate(recipe, self.build_schema)
        self.assertTrue(any("const" in e for e in errs))

    def test_recipe_bad_lang_enum(self):
        recipe = {
            "schema": "cemu.guest-function-build.v1", "module": "x",
            "identity_ref": "i.json", "sources": [{"path": "a.c", "lang": "rust"}],
        }
        errs = schemamod.validate(recipe, self.build_schema)
        self.assertTrue(any("enum" in e for e in errs))

    def test_recipe_additional_property_rejected(self):
        recipe = {
            "schema": "cemu.guest-function-build.v1", "module": "x",
            "identity_ref": "i.json", "sources": [{"path": "a.c", "lang": "c"}],
            "surprise": 1,
        }
        errs = schemamod.validate(recipe, self.build_schema)
        self.assertTrue(any("additional property" in e for e in errs))

    def test_module_name_pattern(self):
        recipe = {
            "schema": "cemu.guest-function-build.v1",
            "module": "bad name with spaces!",
            "identity_ref": "i.json", "sources": [{"path": "a.c", "lang": "c"}],
        }
        errs = schemamod.validate(recipe, self.build_schema)
        self.assertTrue(any("does not match" in e for e in errs))

    def test_manifest_rejects_bad_reloc_type(self):
        manifest = _minimal_manifest()
        manifest["relocations"] = [{
            "section": ".text", "offset": 0, "type": "R_PPC_TLS", "symbol": "x",
            "addend": 0}]
        errs = schemamod.validate(manifest, self.rt_schema)
        self.assertTrue(any("enum" in e for e in errs))

    def test_manifest_rejects_bad_alignment(self):
        manifest = _minimal_manifest()
        manifest["sections"][0]["alignment"] = 32
        errs = schemamod.validate(manifest, self.rt_schema)
        self.assertTrue(any("enum" in e for e in errs))

    def test_manifest_image_path_pattern(self):
        manifest = _minimal_manifest()
        manifest["image"]["path"] = "../escape/image.bin"
        errs = schemamod.validate(manifest, self.rt_schema)
        self.assertTrue(any("does not match" in e for e in errs))


def _minimal_manifest():
    return {
        "schema": "cemu.guest-functions.v1",
        "identity": {
            "title_id": "00050000101C9300", "title_version": 208,
            "module": "u-king", "patch_crc": 0x6267BFD0,
            "source_rpx_sha256": "0" * 64,
        },
        "image": {"path": "image.bin", "file_sha256": "0" * 64,
                  "module_digest": "0" * 64, "size": 16},
        "sections": [{"id": ".text", "kind": "text", "image_offset": 0,
                      "file_size": 16, "memory_size": 16, "alignment": 4}],
        "symbols": [], "imports": [], "relocations": [], "hooks": [],
    }


class ElfReaderRejectionTest(unittest.TestCase):
    def _base_header(self, ei_class=1, ei_data=2, e_type=1, e_machine=20):
        # Build a minimal but structurally-complete ELF32 header + null section.
        # We only need parse_elf to get far enough to hit the specific check.
        ident = bytes([0x7F]) + b"ELF" + bytes([ei_class, ei_data, 1, 0]) + b"\x00" * 8
        # section headers: 1 null section at offset 52
        e_shoff = 52
        hdr = ident + struct.pack(
            ">HHIIIIIHHHHHH",
            e_type, e_machine, 1,      # type, machine, version
            0, 0, e_shoff,             # entry, phoff, shoff
            0, 52, 0,                  # flags, ehsize, phentsize
            0, 40, 1, 0)               # phnum, shentsize, shnum, shstrndx
        # one null section header (40 bytes of zero)
        return hdr + b"\x00" * 40

    def test_bad_magic(self):
        with self.assertRaises(elfmod.ElfError):
            elfmod.parse_elf(b"NOPE" + b"\x00" * 60)

    def test_wrong_class_64(self):
        blob = self._base_header(ei_class=2)
        with self.assertRaises(elfmod.ElfError) as cm:
            elfmod.parse_elf(blob)
        self.assertIn("ELFCLASS32", str(cm.exception))

    def test_wrong_endian_le(self):
        blob = self._base_header(ei_data=1)
        with self.assertRaises(elfmod.ElfError) as cm:
            elfmod.parse_elf(blob)
        self.assertIn("big-endian", str(cm.exception))

    def test_wrong_machine(self):
        blob = self._base_header(e_machine=62)  # x86-64
        with self.assertRaises(elfmod.ElfError) as cm:
            elfmod.parse_elf(blob)
        self.assertIn("EM_PPC", str(cm.exception))

    def test_wrong_type_not_rel(self):
        blob = self._base_header(e_type=2)  # ET_EXEC
        with self.assertRaises(elfmod.ElfError) as cm:
            elfmod.parse_elf(blob)
        self.assertIn("ET_REL", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
