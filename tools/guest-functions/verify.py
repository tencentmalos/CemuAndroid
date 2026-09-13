#!/usr/bin/env python3
"""verify.py -- re-derive a build and confirm it matches the produced package.

Given a recipe and an existing build output, verify.py rebuilds into a fresh
directory and compares the deterministic module digest, the manifest content and
image.bin. It reports which comparisons were performed and which matched. This
is a reproducibility check, not a release-signing system (spec 5.1).
"""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))

import build as buildmod                    # noqa: E402
from gfbuild.toolchain import Toolchain, ToolchainError  # noqa: E402


def _load(p: Path) -> dict:
    return json.loads(p.read_text())


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Verify a guest-function build.")
    ap.add_argument("recipe", help="Path to build.json recipe.")
    ap.add_argument("--build", required=True, help="Existing package directory.")
    ap.add_argument("--identity", required=True, help="Path to identity.json.")
    ap.add_argument("--toolchain", required=True, help="Toolchain root.")
    ap.add_argument("--linker", default=None)
    ap.add_argument("--rpx", default=None)
    ap.add_argument("--module-map", default=None)
    args = ap.parse_args(argv)

    existing = Path(args.build)
    old_manifest = _load(existing / "guest_functions.json")
    old_image = (existing / "image.bin").read_bytes()

    try:
        tc = Toolchain.discover(args.toolchain, args.linker)
    except ToolchainError as e:
        print(f"verify: toolchain error: {e}", file=sys.stderr)
        return 2

    module_map = _load(Path(args.module_map)) if args.module_map else None
    rpx = Path(args.rpx) if args.rpx else None
    driver = buildmod.BuildDriver(Path(args.recipe), Path(args.identity), tc,
                                  module_map, rpx)

    checks = []
    ok = True
    with tempfile.TemporaryDirectory() as td:
        with tempfile.TemporaryDirectory() as out:
            result = driver.run(Path(td), Path(out))
            new_manifest = result["manifest"]
            new_image = (Path(out) / "image.bin").read_bytes()

    def check(name, a, b):
        nonlocal ok
        same = a == b
        checks.append((name, same))
        if not same:
            ok = False

    check("module_digest", old_manifest["content_digest"],
          new_manifest["content_digest"])
    check("image_bytes", old_image, new_image)
    check("sections", old_manifest["sections"], new_manifest["sections"])
    check("relocations", old_manifest["relocations"], new_manifest["relocations"])
    check("symbols", old_manifest["symbols"], new_manifest["symbols"])
    check("hooks", old_manifest["hooks"], new_manifest["hooks"])
    check("identity", old_manifest["identity"], new_manifest["identity"])

    for name, same in checks:
        print(f"  {'OK ' if same else 'FAIL'} {name}")
    print("verify: PASS" if ok else "verify: MISMATCH")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
