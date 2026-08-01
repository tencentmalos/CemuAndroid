#!/usr/bin/env python3
"""Fail-closed identity verification for Cemu Guest RPX and IDA assets."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"{path}: top-level JSON must be an object")
    return value


def normalize_hex(value: object, field: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{field}: expected hexadecimal string")
    try:
        return f"0x{int(value, 0):08X}"
    except ValueError as error:
        raise ValueError(f"{field}: invalid hexadecimal value {value!r}") from error


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify a Cemu guest-executable manifest, RPX, and optional IDA database."
    )
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--rpx", type=Path, required=True)
    parser.add_argument("--analysis-manifest", type=Path)
    parser.add_argument("--ida-database", type=Path)
    parser.add_argument("--expected-title-id")
    parser.add_argument("--expected-title-version", type=int)
    parser.add_argument("--expected-module")
    parser.add_argument("--expected-module-matches")
    args = parser.parse_args()

    if args.ida_database and not args.analysis_manifest:
        parser.error("--ida-database requires --analysis-manifest")

    errors: list[str] = []
    try:
        metadata = load_json(args.metadata)
        require(
            metadata.get("schema") == "cemu.guest-executable.v1",
            "metadata schema is not cemu.guest-executable.v1",
            errors,
        )

        title_id = str(metadata.get("title_id", "")).upper()
        title_version = metadata.get("title_version")
        if args.expected_title_id:
            require(title_id == args.expected_title_id.upper(), "title ID mismatch", errors)
        if args.expected_title_version is not None:
            require(title_version == args.expected_title_version, "title version mismatch", errors)

        rpx_size = args.rpx.stat().st_size
        rpx_sha = sha256(args.rpx)
        require(rpx_size == metadata.get("size_bytes"), "RPX size mismatch", errors)
        require(rpx_sha == str(metadata.get("sha256", "")).lower(), "RPX SHA-256 mismatch", errors)

        modules = metadata.get("modules")
        require(isinstance(modules, list) and bool(modules), "metadata has no modules", errors)
        module: dict[str, Any] | None = None
        if isinstance(modules, list):
            for candidate in modules:
                if not isinstance(candidate, dict):
                    continue
                if args.expected_module is None or candidate.get("name") == args.expected_module:
                    module = candidate
                    break
        require(module is not None, "expected module not found", errors)

        if module is not None:
            if args.expected_module:
                require(module.get("name") == args.expected_module, "module name mismatch", errors)
            if args.expected_module_matches:
                require(
                    normalize_hex(module.get("patch_crc"), "module.patch_crc")
                    == normalize_hex(args.expected_module_matches, "expected moduleMatches"),
                    "moduleMatches mismatch",
                    errors,
                )
            sections = module.get("sections")
            require(isinstance(sections, list) and bool(sections), "module has no section map", errors)
            if isinstance(sections, list):
                linked = [item for item in sections if isinstance(item, dict) and item.get("linked_address")]
                require(bool(linked), "module has no linked section addresses", errors)

        result: dict[str, Any] = {
            "status": "ok",
            "metadata": str(args.metadata.resolve()),
            "metadata_sha256": sha256(args.metadata),
            "rpx": str(args.rpx.resolve()),
            "rpx_sha256": rpx_sha,
            "rpx_size_bytes": rpx_size,
            "title_id": title_id,
            "title_version": title_version,
            "module": module.get("name") if module else None,
            "module_matches": module.get("patch_crc") if module else None,
        }

        if args.analysis_manifest:
            analysis = load_json(args.analysis_manifest)
            require(
                analysis.get("schema") == "cemu.reverse-database.v1",
                "analysis manifest schema is not cemu.reverse-database.v1",
                errors,
            )
            source = analysis.get("source_rpx")
            require(isinstance(source, dict), "analysis manifest has no source_rpx", errors)
            if isinstance(source, dict):
                require(source.get("sha256") == rpx_sha, "analysis source RPX SHA mismatch", errors)
                require(source.get("size_bytes") == rpx_size, "analysis source RPX size mismatch", errors)

            database = analysis.get("ida_database")
            require(isinstance(database, dict), "analysis manifest has no ida_database", errors)
            if args.ida_database and isinstance(database, dict):
                ida_size = args.ida_database.stat().st_size
                ida_sha = sha256(args.ida_database)
                require(ida_size == database.get("size_bytes"), "IDA database size mismatch", errors)
                require(ida_sha == database.get("sha256"), "IDA database SHA-256 mismatch", errors)
                result.update(
                    {
                        "analysis_manifest": str(args.analysis_manifest.resolve()),
                        "analysis_manifest_sha256": sha256(args.analysis_manifest),
                        "ida_database": str(args.ida_database.resolve()),
                        "ida_database_sha256": ida_sha,
                        "ida_database_size_bytes": ida_size,
                    }
                )

        if errors:
            result["status"] = "failed"
            result["errors"] = errors
            print(json.dumps(result, ensure_ascii=False, indent=2))
            return 1

        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(json.dumps({"status": "error", "error": str(error)}, ensure_ascii=False, indent=2))
        return 2


if __name__ == "__main__":
    sys.exit(main())
