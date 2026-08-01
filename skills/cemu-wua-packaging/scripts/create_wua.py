#!/usr/bin/env python3
"""Validate expected title versions, run Cemu's WUA tool, and inspect the result."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

from inspect_tmd import parse_tmd


def title_path(path: Path) -> Path:
    return path / "title.tmd" if path.is_dir() else path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cemu", type=Path, required=True)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--update", type=Path)
    parser.add_argument("--dlc", type=Path)
    parser.add_argument("--update-overlay", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expect-update-version", type=int)
    parser.add_argument("--expect-dlc-version", type=int)
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    try:
        base = parse_tmd(title_path(args.base), True)
        update = parse_tmd(title_path(args.update), True) if args.update else None
        dlc = parse_tmd(title_path(args.dlc), True) if args.dlc else None
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1

    expected_types = ((base, "base"), (update, "update"), (dlc, "dlc"))
    for title, expected_type in expected_types:
        if title and (title["type"] != expected_type or title["validation"] != "ok"):
            print(f"invalid {expected_type} input: {title}", file=sys.stderr)
            return 1
    if args.expect_update_version is not None and (
        not update or update["version"] != args.expect_update_version
    ):
        print(f"expected update v{args.expect_update_version}, got {update}", file=sys.stderr)
        return 1
    if args.expect_dlc_version is not None and (not dlc or dlc["version"] != args.expect_dlc_version):
        print(f"expected DLC v{args.expect_dlc_version}, got {dlc}", file=sys.stderr)
        return 1

    command = [
        str(args.cemu.resolve()),
        "--create-wua",
        str(args.output.resolve()),
        "--wua-base",
        str(title_path(args.base).resolve()),
    ]
    if update:
        command.extend(("--wua-update", str(title_path(args.update).resolve())))
    if dlc:
        command.extend(("--wua-dlc", str(title_path(args.dlc).resolve())))
    if args.update_overlay:
        command.extend(("--wua-update-overlay", str(args.update_overlay.resolve())))
    if args.overwrite:
        command.append("--wua-overwrite")

    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        return result.returncode
    return subprocess.run(
        [str(args.cemu.resolve()), "--inspect-wua", str(args.output.resolve())], check=False
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
