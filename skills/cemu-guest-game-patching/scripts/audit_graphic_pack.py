#!/usr/bin/env python3
"""对 Cemu graphic pack 做只读静态审计。"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


TOKEN_RE = re.compile(r"__[A-Z0-9_]+__")
IMPORT_RE = re.compile(r"\bimport\.([A-Za-z0-9_]+)\.([A-Za-z0-9_.$@]+)")
HEX_RE = re.compile(r"0x[0-9A-Fa-f]+")
PATCH_ADDRESS_RE = re.compile(r"^\s*(0x[0-9A-Fa-f]+)\s*=")
GROUP_RE = re.compile(r"^\s*\[([^]]+)]\s*$")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".mm"}


def strip_comment(line: str) -> str:
    in_string = False
    for index, char in enumerate(line):
        if char == '"':
            in_string = not in_string
        elif char in "#;" and not in_string:
            return line[:index]
    return line


def parse_rules(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {
        "title_ids": [],
        "name": None,
        "virtual_path": None,
        "version": None,
        "default_enabled": False,
    }
    section = ""
    for raw_line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        line = strip_comment(raw_line).strip()
        if not line:
            continue
        group = GROUP_RE.match(line)
        if group:
            section = group.group(1).casefold()
            continue
        if section != "definition" or "=" not in line:
            continue
        key, value = (part.strip() for part in line.split("=", 1))
        value = value.strip('"')
        if key.casefold() == "titleids":
            result["title_ids"] = [item.strip().lower() for item in value.split(",") if item.strip()]
        elif key.casefold() == "name":
            result["name"] = value
        elif key.casefold() == "path":
            result["virtual_path"] = value
        elif key.casefold() == "version":
            result["version"] = value
        elif key.casefold() == "default":
            result["default_enabled"] = value.casefold() in {"1", "true", "yes"}
    return result


def collect_source_text(cemu_root: Path | None) -> str:
    if cemu_root is None:
        return ""
    source_root = cemu_root / "src"
    if not source_root.is_dir():
        raise ValueError(f"Cemu source directory does not exist: {source_root}")
    chunks: list[str] = []
    for path in source_root.rglob("*"):
        relative_parts = path.relative_to(source_root).parts
        if any(part.startswith(".") or part in {"build", "out"} for part in relative_parts[:-1]):
            continue
        if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
            chunks.append(path.read_text(encoding="utf-8", errors="ignore"))
    return "\n".join(chunks)


def audit(pack: Path, cemu_root: Path | None, expected_module: str | None, expected_title: str | None) -> dict[str, Any]:
    pack = pack.resolve()
    rules_path = pack / "rules.txt"
    if not rules_path.is_file():
        raise ValueError(f"rules.txt not found: {rules_path}")

    rules = parse_rules(rules_path)
    patch_files = sorted(pack.glob("patch_*.asm"))
    all_files = [rules_path, *patch_files]
    unresolved_tokens: dict[str, list[str]] = {}
    imports: set[tuple[str, str]] = set()
    module_matches: set[str] = set()
    groups: list[dict[str, Any]] = []
    absolute_addresses: set[str] = set()
    codecave_files: set[str] = set()
    callback_count = 0

    for path in all_files:
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        tokens = sorted(set(TOKEN_RE.findall(text)))
        if tokens:
            unresolved_tokens[path.name] = tokens
        if path == rules_path:
            continue

        current_group: dict[str, Any] | None = None
        for line_number, raw_line in enumerate(text.splitlines(), start=1):
            line = strip_comment(raw_line).strip()
            if not line:
                continue
            group_match = GROUP_RE.match(line)
            if group_match:
                current_group = {
                    "name": group_match.group(1),
                    "file": path.name,
                    "line": line_number,
                    "module_matches": [],
                    "rpx_only": False,
                }
                groups.append(current_group)
                continue
            if re.match(r"(?i)^moduleMatches\s*=\s*(rpx|entry)\b", line):
                if current_group is not None:
                    current_group["rpx_only"] = True
            elif re.match(r"(?i)^moduleMatches\s*=", line):
                values = [value.lower() for value in HEX_RE.findall(line)]
                module_matches.update(values)
                if current_group is not None:
                    current_group["module_matches"].extend(values)
            if re.search(r"(?i)\.origin\s*=\s*codecave", line):
                codecave_files.add(path.name)
            callback_count += len(re.findall(r"(?i)\.callback\b", line))
            for module, function in IMPORT_RE.findall(line):
                imports.add((module, function))
            address = PATCH_ADDRESS_RE.match(line)
            if address:
                absolute_addresses.add(address.group(1).lower())

    source_text = collect_source_text(cemu_root)
    import_rows = []
    for module, function in sorted(imports):
        source_evidence = None if cemu_root is None else function in source_text
        import_rows.append(
            {
                "module": module,
                "function": function,
                "native_source_evidence": source_evidence,
                "is_custom_hook": function.startswith("hook_") or function.startswith("log_"),
            }
        )

    expected_module_normalized = expected_module.lower() if expected_module else None
    if expected_module_normalized and not expected_module_normalized.startswith("0x"):
        expected_module_normalized = f"0x{expected_module_normalized}"
    expected_title_normalized = expected_title.lower().replace("0x", "") if expected_title else None
    groups_without_selector = [group for group in groups if not group["module_matches"] and not group["rpx_only"]]
    missing_hook_evidence = [
        f"{row['module']}.{row['function']}"
        for row in import_rows
        if row["is_custom_hook"] and row["native_source_evidence"] is False
    ]

    errors: list[str] = []
    warnings: list[str] = []
    if not patch_files:
        warnings.append("没有找到 patch_*.asm；该包只能验证 rules/shader/resource 层。")
    if unresolved_tokens:
        errors.append("存在未展开的模板 token，不能把原始 launcher 模板直接当成可运行包。")
    if groups_without_selector:
        errors.append("存在没有 moduleMatches/rpx 选择器的 patch group。")
    if expected_module_normalized and expected_module_normalized not in module_matches:
        errors.append(f"未找到期望的 moduleMatches={expected_module_normalized}。")
    if expected_title_normalized and expected_title_normalized not in rules["title_ids"]:
        errors.append(f"rules.txt 未包含期望 titleId={expected_title_normalized}。")
    if missing_hook_evidence:
        warnings.append("Cemu src 中没有找到部分自定义 HLE hook 名称；运行时可能只会解析到 unsupported-import trampoline。")
    if rules["default_enabled"]:
        warnings.append("graphic pack 默认启用；真机探针前必须备份配置并准备可回退的移除路径。")

    return {
        "pack": str(pack),
        "rules": rules,
        "patch_files": [path.name for path in patch_files],
        "patch_file_count": len(patch_files),
        "groups": groups,
        "group_count": len(groups),
        "module_matches": sorted(module_matches),
        "codecave_files": sorted(codecave_files),
        "absolute_patch_addresses": sorted(absolute_addresses),
        "absolute_patch_address_count": len(absolute_addresses),
        "callback_count": callback_count,
        "imports": import_rows,
        "custom_hook_import_count": sum(1 for row in import_rows if row["is_custom_hook"]),
        "missing_custom_hook_source_evidence": missing_hook_evidence,
        "unresolved_tokens": unresolved_tokens,
        "errors": errors,
        "warnings": warnings,
    }


def print_text(report: dict[str, Any]) -> None:
    rules = report["rules"]
    print(f"Pack: {rules['name'] or '--'}")
    print(f"Path: {report['pack']}")
    print(f"Title IDs: {', '.join(rules['title_ids']) or '--'}")
    print(f"Format version: {rules['version'] or '--'}; default enabled: {rules['default_enabled']}")
    print(f"Patch files/groups: {report['patch_file_count']}/{report['group_count']}")
    print(f"moduleMatches: {', '.join(report['module_matches']) or '--'}")
    print(f"Codecave files: {len(report['codecave_files'])}")
    print(f"Absolute patch addresses: {report['absolute_patch_address_count']}")
    print(f"Imports/custom hooks: {len(report['imports'])}/{report['custom_hook_import_count']}")
    source_check = "not checked" if all(row["native_source_evidence"] is None for row in report["imports"]) else len(report["missing_custom_hook_source_evidence"])
    print(f"Missing custom-hook source evidence: {source_check}")
    print(f"Unresolved template files: {len(report['unresolved_tokens'])}")
    for message in report["errors"]:
        print(f"ERROR: {message}")
    for message in report["warnings"]:
        print(f"WARN: {message}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit a Cemu graphic pack without modifying it.")
    parser.add_argument("pack", type=Path, help="Directory containing rules.txt")
    parser.add_argument("--cemu-root", type=Path, help="Optional Cemu source root used to find native hook evidence")
    parser.add_argument("--expected-module", help="Expected moduleMatches value, for example 0x6267BFD0")
    parser.add_argument("--expected-title", help="Expected 16-digit Wii U title ID")
    parser.add_argument("--format", choices=("text", "json"), default="text")
    parser.add_argument("--strict", action="store_true", help="Return non-zero for warnings as well as errors")
    args = parser.parse_args()

    try:
        report = audit(args.pack, args.cemu_root, args.expected_module, args.expected_title)
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2

    if args.format == "json":
        print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
    else:
        print_text(report)

    if report["errors"]:
        return 2
    if args.strict and report["warnings"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
