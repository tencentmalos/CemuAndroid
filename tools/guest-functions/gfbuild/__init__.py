"""Cemu guest-function build tooling.

This package implements the offline half of the Guest Function Patch / Custom
SDK pipeline defined in
docs/plans/2026-09-13-guest-function-patch-custom-sdk-spec.md.

It is deliberately dependency-free (standard library only). The ELF reader,
relocation engine and schema validation are re-implemented from scratch so the
build side and the Cemu C++ loader can be validated against the same golden
fixtures rather than sharing a parser that could mask divergence.
"""

SCHEMA_BUILD = "cemu.guest-function-build.v1"
SCHEMA_RUNTIME = "cemu.guest-functions.v1"
SDK_SOURCE_VERSION = "guest/custom/v1"

__all__ = [
    "SCHEMA_BUILD",
    "SCHEMA_RUNTIME",
    "SDK_SOURCE_VERSION",
]
