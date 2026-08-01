#!/usr/bin/env python3
"""Inspect Wii U TMD metadata and optionally validate its NUS content files."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys


TMD_HEADER_SIZE = 0xB04
CONTENT_ENTRY_SIZE = 0x30
TITLE_TYPES = {
    0x00: "base",
    0x02: "demo",
    0x0C: "dlc",
    0x0E: "update",
    0x0F: "homebrew",
    0x10: "system",
    0x1B: "system-data",
    0x30: "system-overlay",
}


def parse_tmd(input_path: Path, verify_files: bool) -> dict[str, object]:
    tmd_path = input_path / "title.tmd" if input_path.is_dir() else input_path
    data = tmd_path.read_bytes()
    if len(data) < TMD_HEADER_SIZE:
        raise ValueError(f"{tmd_path}: TMD is shorter than 0x{TMD_HEADER_SIZE:x} bytes")

    title_id = struct.unpack_from(">Q", data, 0x18C)[0]
    version, content_count = struct.unpack_from(">HH", data, 0x1DC)
    expected_size = TMD_HEADER_SIZE + content_count * CONTENT_ENTRY_SIZE
    if len(data) < expected_size:
        raise ValueError(f"{tmd_path}: TMD is truncated ({len(data)} < {expected_size})")

    errors: list[str] = []
    payload_size = 0
    for index in range(content_count):
        offset = TMD_HEADER_SIZE + index * CONTENT_ENTRY_SIZE
        content_id, _content_index, flags, size = struct.unpack_from(">IHHQ", data, offset)
        payload_size += size
        if not verify_files:
            continue

        app_path = tmd_path.parent / f"{content_id:08x}.app"
        if not app_path.is_file():
            errors.append(f"missing {app_path.name}")
        elif app_path.stat().st_size != size:
            errors.append(f"{app_path.name}: size {app_path.stat().st_size} != {size}")

        if flags & 0x02:
            h3_path = tmd_path.parent / f"{content_id:08x}.h3"
            expected_hash = data[offset + 0x10 : offset + 0x24]
            if not h3_path.is_file():
                errors.append(f"missing {h3_path.name}")
            elif hashlib.sha1(h3_path.read_bytes()).digest() != expected_hash:
                errors.append(f"{h3_path.name}: SHA-1 mismatch")

    if verify_files:
        for required_name in ("title.tik", "title.cert"):
            if not (tmd_path.parent / required_name).is_file():
                errors.append(f"missing {required_name}")

    title_type_byte = (title_id >> 32) & 0xFF
    return {
        "path": str(tmd_path.resolve()),
        "title_id": f"{title_id:016x}",
        "type": TITLE_TYPES.get(title_type_byte, "unknown"),
        "version": version,
        "content_count": content_count,
        "payload_bytes": payload_size,
        "validation": "ok" if not errors else "failed",
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="+", type=Path, help="title.tmd file or containing directory")
    parser.add_argument("--verify-files", action="store_true", help="validate APP sizes and H3 hashes")
    parser.add_argument("--json", action="store_true", help="emit JSON")
    args = parser.parse_args()

    try:
        results = [parse_tmd(path, args.verify_files) for path in args.paths]
    except (OSError, ValueError, struct.error) as error:
        print(error, file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(results, ensure_ascii=False, indent=2))
    else:
        for result in results:
            print(
                f"{result['type']}: {result['title_id']} v{result['version']} "
                f"contents={result['content_count']} validation={result['validation']}"
            )
            print(f"  {result['path']}")
            for error in result["errors"]:
                print(f"  ERROR: {error}")

    return 0 if all(result["validation"] == "ok" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
