"""End-to-end build tests: real toolchain -> package -> verify.

These require a PPC32 BE toolchain. The path is taken from the
CEMU_GUEST_TOOLCHAIN / CEMU_GUEST_LINKER environment variables, or the doctor
receipt at CEMU_GUEST_RECEIPT. If none resolve, the tests skip (they do not
fail) so the pure-Python suite stays runnable without LLVM.

The digest-sensitivity test proves spec 11/G0's "direct/indirect header
modification is detectable": changing a source constant changes image bytes and
thus the module digest; the digest is stable across rebuilds otherwise.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_ROOT))

from gfbuild.toolchain import Toolchain, ToolchainError  # noqa: E402
import build as buildmod                                 # noqa: E402


def _resolve_toolchain():
    root = os.environ.get("CEMU_GUEST_TOOLCHAIN")
    linker = os.environ.get("CEMU_GUEST_LINKER")
    if not root:
        receipt = os.environ.get("CEMU_GUEST_RECEIPT")
        if receipt and Path(receipt).exists():
            data = json.loads(Path(receipt).read_text())
            root = data.get("root")
            linker = data.get("tools", {}).get("linker", {}).get("path")
    if not root:
        # Common local location; skip if absent.
        for cand in ("/opt/homebrew/opt/llvm", "/usr/local/opt/llvm"):
            if Path(cand).exists():
                root = cand
                break
        if linker is None:
            for cand in ("/opt/homebrew/opt/lld/bin/ld.lld",):
                if Path(cand).exists():
                    linker = cand
                    break
    if not root:
        return None
    try:
        return Toolchain.discover(root, linker)
    except ToolchainError:
        return None


TC = _resolve_toolchain()

IDENTITY = {
    "title_id": "00050000101C9300",
    "region": "JP",
    "title_version": 208,
    "dlc_version": 80,
    "module": "u-king",
    "module_matches": "0x6267BFD0",
    "source_rpx": {"sha256": "ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30"},
}

MODULE_MAP = {
    "segments": [
        {"name": ".text", "start": 0x02000000, "size": 0x00800000},
        {"name": ".data", "start": 0x10000000, "size": 0x00400000},
    ]
}

SOURCE = r"""
extern int orig_calc(int);
static const int kBias = %BIAS%;
int hooked_calc(int x) { return orig_calc(x) + kBias; }
int pure_double(int x) { return x * 2; }
"""

RECIPE = {
    "schema": "cemu.guest-function-build.v1",
    "module": "u-king",
    "identity_ref": "identity.json",
    "sources": [{"path": "mod.c", "lang": "c"}],
    "imports": [
        {"symbol": "orig_calc", "kind": "guest_function", "guest_va": 0x02001000,
         "abi_evidence": "test-fixture"}
    ],
    "hooks": [
        {"kind": "entry", "guest_va": 0x02001000, "target_symbol": "hooked_calc",
         "original_policy": "trampoline", "original_symbol": "orig_calc",
         "original_bytes": "9421ffd0"}  # stwu r1,-0x30(r1)
    ],
}


@unittest.skipIf(TC is None, "no PPC toolchain available")
class EndToEndBuildTest(unittest.TestCase):
    def _write_case(self, d: Path, bias: int) -> Path:
        (d / "identity.json").write_text(json.dumps(IDENTITY))
        (d / "mod.c").write_text(SOURCE.replace("%BIAS%", str(bias)))
        recipe_path = d / "build.json"
        recipe_path.write_text(json.dumps(RECIPE))
        return recipe_path

    def _build(self, d: Path, bias: int) -> dict:
        recipe = self._write_case(d, bias)
        driver = buildmod.BuildDriver(recipe, d / "identity.json", TC,
                                      MODULE_MAP, None)
        out = d / "out"
        with tempfile.TemporaryDirectory() as td:
            return driver.run(Path(td), out)

    def test_build_produces_manifest_and_image(self):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            result = self._build(d, 7)
            m = result["manifest"]
            self.assertEqual(m["schema"], "cemu.guest-functions.v1")
            self.assertEqual(m["identity"]["title_id"], "00050000101C9300")
            self.assertEqual(m["identity"]["patch_crc"], 0x6267BFD0)
            names = {s["name"] for s in m["symbols"]}
            self.assertIn("hooked_calc", names)
            self.assertIn("pure_double", names)
            self.assertTrue((d / "out" / "image.bin").exists())
            self.assertTrue((d / "out" / "guest_functions.json").exists())
            # image.bin sha matches manifest
            img = (d / "out" / "image.bin").read_bytes()
            import hashlib
            self.assertEqual(m["image"]["file_sha256"], hashlib.sha256(img).hexdigest())

    def test_digest_reproducible_same_input(self):
        with tempfile.TemporaryDirectory() as td1, tempfile.TemporaryDirectory() as td2:
            r1 = self._build(Path(td1), 7)
            r2 = self._build(Path(td2), 7)
            self.assertEqual(r1["digest"], r2["digest"],
                             "same input must yield same digest")

    def test_digest_changes_on_source_change(self):
        # Indirect header change: a different constant changes image bytes.
        with tempfile.TemporaryDirectory() as td1, tempfile.TemporaryDirectory() as td2:
            r1 = self._build(Path(td1), 7)
            r2 = self._build(Path(td2), 999)
            self.assertNotEqual(r1["digest"], r2["digest"],
                                "changed constant must change the digest")

    def test_manifest_validates_against_schema(self):
        import gfbuild.schema as schemamod
        schema = schemamod.load_schema(_ROOT / "schema" / "runtime.manifest.v1.schema.json")
        with tempfile.TemporaryDirectory() as td:
            result = self._build(Path(td), 7)
            errors = schemamod.validate(result["manifest"], schema)
            self.assertEqual(errors, [], f"manifest schema errors: {errors}")


@unittest.skipIf(TC is None, "no PPC toolchain available")
class RejectionTest(unittest.TestCase):
    def test_rejects_undeclared_import(self):
        src = "extern int mystery(int); int f(int x){return mystery(x);}\n"
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            (d / "identity.json").write_text(json.dumps(IDENTITY))
            (d / "mod.c").write_text(src)
            recipe = dict(RECIPE)
            recipe = {**RECIPE, "imports": [], "hooks": []}
            (d / "build.json").write_text(json.dumps(recipe))
            driver = buildmod.BuildDriver(d / "build.json", d / "identity.json",
                                          TC, MODULE_MAP, None)
            from gfbuild.module import BuildError
            with tempfile.TemporaryDirectory() as work:
                with self.assertRaises(BuildError):
                    driver.run(Path(work), d / "out")

    def test_rejects_hook_va_outside_module_map(self):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            (d / "identity.json").write_text(json.dumps(IDENTITY))
            (d / "mod.c").write_text(SOURCE.replace("%BIAS%", "1"))
            recipe = {**RECIPE, "hooks": [
                {"kind": "entry", "guest_va": 0x09999999,
                 "target_symbol": "hooked_calc", "original_policy": "none",
                 "original_bytes": "9421ffd0"}]}
            (d / "build.json").write_text(json.dumps(recipe))
            driver = buildmod.BuildDriver(d / "build.json", d / "identity.json",
                                          TC, MODULE_MAP, None)
            with tempfile.TemporaryDirectory() as work:
                with self.assertRaises(buildmod.BuildErrorCLI):
                    driver.run(Path(work), d / "out")


if __name__ == "__main__":
    unittest.main()
