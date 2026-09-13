"""Minimal, dependency-free JSON Schema validator.

Supports exactly the draft-07 constructs used by the guest-function schemas:
type, required, properties, additionalProperties, enum, const, pattern,
minimum/maximum, minItems, items, and $id lookup. It is deliberately small and
strict: an unsupported schema keyword is an error, not a silent pass, so the
schema files cannot drift into using something this validator ignores.

The same schemas are meant to be honored by the Cemu C++ loader; keeping the
validator explicit (rather than pulling a full library) makes the shared
contract auditable.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any, Dict, List

_SUPPORTED_KEYWORDS = {
    "$schema", "$id", "title", "description", "type", "required", "properties",
    "additionalProperties", "enum", "const", "pattern", "minimum", "maximum",
    "minItems", "maxItems", "items", "default",
}

_TYPE_CHECKS = {
    "object": lambda v: isinstance(v, dict),
    "array": lambda v: isinstance(v, list),
    "string": lambda v: isinstance(v, str),
    "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
    "boolean": lambda v: isinstance(v, bool),
    "null": lambda v: v is None,
}


class SchemaError(Exception):
    """Raised when validation fails; message includes the JSON path."""


def load_schema(path: str | Path) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _check_keywords(schema: Dict[str, Any], where: str) -> None:
    for k in schema:
        if k not in _SUPPORTED_KEYWORDS:
            raise SchemaError(f"schema uses unsupported keyword '{k}' at {where}")


def _validate(value: Any, schema: Dict[str, Any], path: str,
              errors: List[str]) -> None:
    _check_keywords(schema, path or "<root>")

    if "const" in schema and value != schema["const"]:
        errors.append(f"{path}: expected const {schema['const']!r}, got {value!r}")
        return

    if "type" in schema:
        t = schema["type"]
        types = t if isinstance(t, list) else [t]
        if not any(_TYPE_CHECKS[tt](value) for tt in types):
            errors.append(f"{path}: expected type {t}, got {type(value).__name__}")
            return

    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{path}: {value!r} not in enum {schema['enum']}")

    if isinstance(value, str) and "pattern" in schema:
        if re.fullmatch(schema["pattern"], value) is None:
            errors.append(f"{path}: {value!r} does not match /{schema['pattern']}/")

    if isinstance(value, bool):
        pass  # bools are not numbers here
    elif isinstance(value, (int, float)):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append(f"{path}: {value} < minimum {schema['minimum']}")
        if "maximum" in schema and value > schema["maximum"]:
            errors.append(f"{path}: {value} > maximum {schema['maximum']}")

    if isinstance(value, dict):
        props = schema.get("properties", {})
        for req in schema.get("required", []):
            if req not in value:
                errors.append(f"{path}: missing required property '{req}'")
        addl = schema.get("additionalProperties", True)
        for key, sub in value.items():
            child_path = f"{path}.{key}" if path else key
            if key in props:
                _validate(sub, props[key], child_path, errors)
            elif addl is False:
                errors.append(f"{child_path}: additional property not allowed")
            elif isinstance(addl, dict):
                _validate(sub, addl, child_path, errors)

    if isinstance(value, list):
        if "minItems" in schema and len(value) < schema["minItems"]:
            errors.append(f"{path}: needs >= {schema['minItems']} items, got {len(value)}")
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            errors.append(f"{path}: needs <= {schema['maxItems']} items, got {len(value)}")
        item_schema = schema.get("items")
        if isinstance(item_schema, dict):
            for i, item in enumerate(value):
                _validate(item, item_schema, f"{path}[{i}]", errors)


def validate(value: Any, schema: Dict[str, Any]) -> List[str]:
    """Return a list of human-readable error strings (empty == valid)."""
    errors: List[str] = []
    _validate(value, schema, "", errors)
    return errors


def validate_or_raise(value: Any, schema: Dict[str, Any]) -> None:
    errors = validate(value, schema)
    if errors:
        raise SchemaError("; ".join(errors))
