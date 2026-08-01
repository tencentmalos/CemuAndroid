#!/usr/bin/env python3
"""Install an SD Cafiine-style localization overlay as a Cemu graphic pack."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import shutil
import sys


REQUIRED_FILES = (
    Path("content/Font/Font_jp.sbfarc"),
    Path("content/Pack/Bootup.pack"),
    Path("content/Pack/Bootup_JPja.pack"),
    Path("content/Pack/Title.pack"),
)


def default_cemu_user_dir() -> Path:
    if sys.platform == "darwin":
        return Path.home() / "Library/Application Support/Cemu"
    return Path.home() / ".local/share/Cemu"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as input_file:
        while chunk := input_file.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path, help="directory containing the patch's content folder")
    parser.add_argument("--cemu-user-dir", type=Path, default=default_cemu_user_dir())
    parser.add_argument("--pack-name", default="BotW_Simplified_Chinese")
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    source = args.source.resolve()
    missing = [relative for relative in REQUIRED_FILES if not (source / relative).is_file()]
    if missing:
        for relative in missing:
            print(f"missing localization file: {source / relative}", file=sys.stderr)
        return 1

    destination = (
        args.cemu_user_dir.expanduser().resolve()
        / "graphicPacks/customGraphicPacks"
        / args.pack_name
    )
    if destination.exists() and not args.overwrite:
        print(f"graphic pack already exists: {destination}; pass --overwrite to update it", file=sys.stderr)
        return 1

    destination.mkdir(parents=True, exist_ok=True)
    for relative in REQUIRED_FILES:
        output_path = destination / relative
        output_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / relative, output_path)

    rules = """[Definition]
titleIds = 00050000101C9300
name = 塞尔达传说：旷野之息 简体中文
path = "The Legend of Zelda: Breath of the Wild/Languages/简体中文"
description = 日版 v208 资源覆盖；不要同时启用其他语言包。
version = 4
default = true
"""
    (destination / "rules.txt").write_text(rules, encoding="utf-8")
    manifest = "\n".join(
        f"{sha256(destination / relative)}  {relative.as_posix()}" for relative in REQUIRED_FILES
    )
    (destination / "manifest.sha256").write_text(manifest + "\n", encoding="utf-8")

    print(f"installed={destination}")
    print("title_id=00050000101c9300 language=简体中文 default_enabled=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
