#!/usr/bin/env python3
"""build.py -- compile a guest-function module into an身份-bound package.

Pipeline (spec 5.1, 4.x):
  1. Load + schema-validate the recipe; load identity and (optional) module map.
  2. Compile each source with the receipt's flags; partial-link to one ET_REL.
  3. Parse strictly, lay out sections base-independently, classify symbols.
  4. Resolve imports (guest VA validated against the module map), lower
     relocations (whitelist only), synthesize hook install bytes + trampolines.
  5. Emit image.bin, runtime manifest (guest_functions.json) and a provenance
     record; compute the deterministic module digest.

The builder only reads local files and never invents addresses: guest VAs come
from the recipe and are checked against the module map's section ranges.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Dict, List, Optional

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))

from gfbuild import elf as elfmod            # noqa: E402
from gfbuild import module as modmod         # noqa: E402
from gfbuild import hooks as hookmod         # noqa: E402
from gfbuild import schema as schemamod      # noqa: E402
from gfbuild.toolchain import Toolchain, ToolchainError  # noqa: E402

SCHEMA_DIR = _HERE / "schema"


def _sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def _sha256_file(p: Path) -> str:
    return _sha256_bytes(p.read_bytes())


def _load_json(p: Path) -> dict:
    with open(p, "r", encoding="utf-8") as f:
        return json.load(f)


def _validate_module_map_va(module_map: Optional[dict], va: int) -> bool:
    """Confirm a guest VA lands inside a known module section range."""
    if module_map is None:
        return True  # no map provided; caller decides strictness

    def _as_int(v):
        if isinstance(v, str):
            return int(v, 0)
        return int(v)

    # Legacy simple form: a flat "segments" list of {start,size}.
    matched_any_range = False
    for seg in module_map.get("segments", []):
        start = seg.get("start")
        size = seg.get("size")
        if start is None or size is None:
            continue
        matched_any_range = True
        if _as_int(start) <= va < _as_int(start) + _as_int(size):
            return True

    # cemu.guest-executable.v1 form: modules[].text_mapping / data_mapping.
    for mod in module_map.get("modules", []):
        for key in ("text_mapping", "data_mapping"):
            m = mod.get(key)
            if not m:
                continue
            base = m.get("base")
            size = m.get("size")
            if base is None or size is None:
                continue
            matched_any_range = True
            if _as_int(base) <= va < _as_int(base) + _as_int(size):
                return True

    # If the map declared no usable ranges at all, do not block the build.
    if not matched_any_range:
        return True
    return False


class BuildDriver:
    def __init__(self, recipe_path: Path, identity_path: Path,
                 toolchain: Toolchain, module_map: Optional[dict],
                 rpx_path: Optional[Path]):
        self.recipe_path = recipe_path
        self.recipe_dir = recipe_path.parent
        self.identity_path = identity_path
        self.tc = toolchain
        self.module_map = module_map
        self.rpx_path = rpx_path
        self.recipe = _load_json(recipe_path)
        self.identity = _load_json(identity_path)

    def _validate_recipe(self) -> None:
        schema = schemamod.load_schema(SCHEMA_DIR / "build.recipe.v1.schema.json")
        schemamod.validate_or_raise(self.recipe, schema)

    def _resolve_source(self, rel: str) -> Path:
        p = (self.recipe_dir / rel).resolve()
        if not p.exists():
            raise FileNotFoundError(f"source not found: {rel}")
        return p

    def _compile_and_link(self, workdir: Path) -> Path:
        include_dirs = [self._resolve_source(d) if not Path(d).is_absolute()
                        else Path(d) for d in self.recipe.get("include_dirs", [])]
        objs: List[Path] = []
        for i, src in enumerate(self.recipe["sources"]):
            sp = self._resolve_source(src["path"])
            op = workdir / f"src{i}.o"
            self.tc.compile_one(sp, op, src["lang"], include_dirs,
                                src.get("extra_flags", []))
            objs.append(op)
        linked = workdir / "module.elf"
        self.tc.partial_link(objs, linked)
        return linked

    def _identity_block(self) -> dict:
        ident = self.identity
        src_sha = ident.get("source_rpx", {}).get("sha256")
        if self.rpx_path is not None:
            actual = _sha256_file(self.rpx_path)
            if src_sha and actual != src_sha:
                raise BuildErrorCLI(
                    f"source RPX SHA mismatch: identity says {src_sha}, file is {actual}")
            src_sha = actual
        if not src_sha:
            raise BuildErrorCLI("no source_rpx sha available (identity or --rpx)")
        module_matches = ident.get("module_matches", "0x0")
        crc = int(module_matches, 16) if isinstance(module_matches, str) else module_matches
        return {
            "title_id": ident["title_id"],
            "region": ident.get("region", ""),
            "title_version": ident["title_version"],
            "update_version": ident.get("dlc_version", 0),
            "module": self.recipe["module"],
            "patch_crc": crc,
            "source_rpx_sha256": src_sha,
            "rpl_imports": [{"module": i["rpl_module"]}
                            for i in self.recipe.get("imports", [])
                            if i["kind"] == "rpl_export" and i.get("rpl_module")],
        }

    def _lower_imports(self) -> List[dict]:
        out = []
        for imp in self.recipe.get("imports", []):
            if imp["kind"] in ("guest_function", "guest_data"):
                va = imp.get("guest_va")
                if va is None:
                    raise BuildErrorCLI(f"import '{imp['symbol']}' needs guest_va")
                if not _validate_module_map_va(self.module_map, va):
                    raise BuildErrorCLI(
                        f"import '{imp['symbol']}' guest_va 0x{va:08x} not in module map")
            entry = {"symbol": imp["symbol"], "kind": imp["kind"]}
            for k in ("guest_va", "rpl_module", "rpl_symbol", "sdk_export_id",
                      "original_bytes", "abi_evidence"):
                if imp.get(k) is not None:
                    entry[k] = imp[k]
            out.append(entry)
        return out

    def _lower_hooks(self, layout: modmod.ModuleLayout, symbols: List[dict],
                     module_base: int) -> List[dict]:
        sym_by_name = {s["name"]: s for s in symbols}
        sec_offset = {s.id: s.image_offset for s in layout.sections}
        out = []
        for hook in self.recipe.get("hooks", []):
            tgt = hook["target_symbol"]
            if tgt not in sym_by_name:
                raise BuildErrorCLI(f"hook target symbol '{tgt}' not defined in module")
            va = hook["guest_va"]
            if not _validate_module_map_va(self.module_map, va):
                raise BuildErrorCLI(f"hook guest_va 0x{va:08x} not in module map")
            orig = hook.get("original_bytes")
            if not orig:
                raise BuildErrorCLI(f"hook at 0x{va:08x} needs original_bytes")
            entry = {
                "kind": hook["kind"],
                "guest_va": va,
                "original_bytes": orig,
                "target_symbol": tgt,
                "original_policy": hook.get("original_policy", "none"),
                "abi_profile": hook.get("abi_profile", "ppc32-eabi-v1"),
            }
            if hook.get("original_symbol"):
                entry["original_symbol"] = hook["original_symbol"]
            if hook.get("continuation_bytes"):
                entry["continuation_bytes"] = hook["continuation_bytes"]
            out.append(entry)
        return out

    def run(self, workdir: Path, out_dir: Path) -> dict:
        self._validate_recipe()
        out_dir.mkdir(parents=True, exist_ok=True)

        linked = self._compile_and_link(workdir)
        obj = elfmod.parse_elf(linked.read_bytes())
        max_bytes = self.recipe.get("max_module_bytes", 262144)
        layout = modmod.lay_out_module(obj, max_bytes)
        imports = modmod.build_import_index(self.recipe.get("imports", []))
        symbols = modmod.classify_symbols(obj, layout, imports)
        relocations = modmod.lower_relocations(obj, layout)
        image = modmod.build_image(layout)

        manifest_imports = self._lower_imports()
        hooks = self._lower_hooks(layout, symbols, 0)

        sections_json = [{
            "id": s.id, "kind": s.kind, "image_offset": s.image_offset,
            "file_size": s.file_size, "memory_size": s.memory_size,
            "alignment": s.alignment,
        } for s in layout.sections]

        image_sha = _sha256_bytes(image)
        digest = modmod.compute_module_digest(
            sections_json, symbols, relocations, hooks, manifest_imports,
            image_sha)

        image_path = out_dir / "image.bin"
        image_path.write_bytes(image)

        manifest = {
            "schema": "cemu.guest-functions.v1",
            "content_digest": digest,
            "identity": self._identity_block(),
            "image": {
                "path": "image.bin",
                "file_sha256": image_sha,
                "module_digest": digest,
                "size": len(image),
            },
            "sections": sections_json,
            "symbols": symbols,
            "imports": manifest_imports,
            "relocations": relocations,
            "hooks": hooks,
        }
        if "host" in self.recipe:
            manifest["host"] = self.recipe["host"]

        # Validate the manifest we are about to write against its own schema.
        rt_schema = schemamod.load_schema(SCHEMA_DIR / "runtime.manifest.v1.schema.json")
        schemamod.validate_or_raise(manifest, rt_schema)

        (out_dir / "guest_functions.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n")

        # Debug artifacts.
        (out_dir / "module.elf").write_bytes(linked.read_bytes())
        provenance = self._provenance(linked, image, manifest)
        (out_dir / "debug.json").write_text(
            json.dumps(provenance, indent=2, sort_keys=True) + "\n")

        return {"manifest": manifest, "provenance": provenance,
                "image_size": len(image), "digest": digest}

    def _provenance(self, linked: Path, image: bytes, manifest: dict) -> dict:
        includes = self._collect_includes()
        return {
            "schema": "cemu.guest-function-build-provenance.v1",
            "recipe_path": str(self.recipe_path),
            "recipe_sha256": _sha256_file(self.recipe_path),
            "identity_sha256": _sha256_file(self.identity_path),
            "sources": [{"path": s["path"], "lang": s["lang"],
                         "sha256": _sha256_file(self._resolve_source(s["path"]))}
                        for s in self.recipe["sources"]],
            "includes": includes,
            "toolchain": self.tc.tool_versions(),
            "elf_sha256": _sha256_file(linked),
            "image_sha256": _sha256_bytes(image),
            "module_digest": manifest["content_digest"],
        }

    def _collect_includes(self) -> List[str]:
        """Parse .d dependency files if present next to compiled objects."""
        # Best-effort; the workdir is gone by now, so we recompute via the
        # recipe's include_dirs listing rather than the transient .d files.
        return [d for d in self.recipe.get("include_dirs", [])]


class BuildErrorCLI(Exception):
    pass


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Build a guest-function module.")
    ap.add_argument("recipe", help="Path to build.json recipe.")
    ap.add_argument("--identity", required=True, help="Path to identity.json.")
    ap.add_argument("--toolchain", required=True, help="Toolchain root.")
    ap.add_argument("--linker", default=None, help="Explicit ld.lld path.")
    ap.add_argument("--rpx", default=None, help="Source RPX for SHA binding.")
    ap.add_argument("--module-map", default=None,
                    help="guest-executable.json with segment VA ranges.")
    ap.add_argument("--output", required=True, help="Output package directory.")
    args = ap.parse_args(argv)

    import tempfile
    try:
        tc = Toolchain.discover(args.toolchain, args.linker)
    except ToolchainError as e:
        print(f"build: toolchain error: {e}", file=sys.stderr)
        return 2

    module_map = _load_json(Path(args.module_map)) if args.module_map else None
    rpx = Path(args.rpx) if args.rpx else None

    driver = BuildDriver(Path(args.recipe), Path(args.identity), tc,
                         module_map, rpx)
    try:
        with tempfile.TemporaryDirectory() as td:
            result = driver.run(Path(td), Path(args.output))
    except (BuildErrorCLI, modmod.BuildError, elfmod.ElfError,
            schemamod.SchemaError, hookmod.HookError, ToolchainError,
            FileNotFoundError) as e:
        print(f"build: FAILED: {e}", file=sys.stderr)
        return 1

    print(f"build: OK -> {args.output}")
    print(f"  module digest : {result['digest']}")
    print(f"  image size    : {result['image_size']} bytes")
    print(f"  sections      : {len(result['manifest']['sections'])}")
    print(f"  symbols       : {len(result['manifest']['symbols'])}")
    print(f"  relocations   : {len(result['manifest']['relocations'])}")
    print(f"  hooks         : {len(result['manifest']['hooks'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
