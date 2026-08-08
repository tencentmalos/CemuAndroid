#!/usr/bin/env python3

"""Summarize RenderDoc actions and EventGPUDuration TSV files."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from collections import Counter
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--actions", required=True, type=Path)
    parser.add_argument("--durations", required=True, type=Path)
    parser.add_argument(
        "--pass-state",
        type=Path,
        help="optional first-draw pipeline/attachment state exported by fetch_gpu_durations",
    )
    return parser.parse_args()


def attachment_signature(row: dict[str, str]) -> str:
    return f'{row["color_targets"]}|{row["depth_target"]}'


def signature_id(signature: str) -> str:
    return hashlib.sha256(signature.encode()).hexdigest()[:12]


def summarize_pass_state(path: Path) -> dict[str, object]:
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source, delimiter="\t"))

    by_reason: dict[str, Counter[str]] = {}
    signatures: dict[str, Counter[str]] = {}
    same_attachment_next: Counter[str] = Counter()
    same_attachment_and_viewport_next: Counter[str] = Counter()
    feedback_after_self_dependency = Counter()
    feedback_pixel_shaders: set[str] = set()

    for index, row in enumerate(rows):
        reason = row["reason"]
        draws = int(row["draw_count"])
        reason_summary = by_reason.setdefault(reason, Counter())
        reason_summary["passes"] += 1
        reason_summary["draws"] += draws
        reason_summary["one_draw"] += draws == 1
        reason_summary["at_most_four_draws"] += draws <= 4

        signature = attachment_signature(row)
        signature_summary = signatures.setdefault(signature, Counter())
        signature_summary["passes"] += 1
        signature_summary["draws"] += draws

        if index + 1 >= len(rows):
            continue
        next_row = rows[index + 1]
        if reason == "self_dependency":
            feedback_after_self_dependency["passes"] += 1
            if next_row.get("feedback_targets"):
                feedback_after_self_dependency["confirmed_feedback"] += 1
                feedback_pixel_shaders.add(next_row["pixel_shader"])
            else:
                feedback_after_self_dependency["no_feedback_in_first_draw"] += 1
        if signature != attachment_signature(next_row):
            continue
        same_attachment_next[reason] += 1
        viewport = (row["viewport_x"], row["viewport_y"], row["viewport_width"], row["viewport_height"])
        next_viewport = (
            next_row["viewport_x"], next_row["viewport_y"],
            next_row["viewport_width"], next_row["viewport_height"],
        )
        if viewport == next_viewport:
            same_attachment_and_viewport_next[reason] += 1

    alternating_attachment_returns = sum(
        attachment_signature(rows[index]) == attachment_signature(rows[index + 2])
        and attachment_signature(rows[index]) != attachment_signature(rows[index + 1])
        for index in range(max(0, len(rows) - 2))
    )
    most_common_signatures = sorted(
        signatures.items(),
        key=lambda item: (item[1]["passes"], item[1]["draws"]),
        reverse=True,
    )[:12]

    return {
        "rows_with_draws": len(rows),
        "by_end_reason": {key: dict(value) for key, value in sorted(by_reason.items())},
        "same_attachment_next_by_end_reason": dict(same_attachment_next),
        "same_attachment_and_viewport_next_by_end_reason": dict(
            same_attachment_and_viewport_next
        ),
        "alternating_attachment_returns_a_b_a": alternating_attachment_returns,
        "feedback_after_self_dependency": {
            **dict(feedback_after_self_dependency),
            "unique_pixel_shaders": len(feedback_pixel_shaders),
        },
        "dominant_attachment_signatures": [
            {
                "id": signature_id(signature),
                "passes": summary["passes"],
                "draws": summary["draws"],
                "attachments": signature,
            }
            for signature, summary in most_common_signatures
        ],
    }


def main() -> int:
    args = parse_args()
    durations: dict[int, float] = {}
    with args.durations.open(newline="") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            durations[int(row["event_id"])] = float(row["duration_seconds"]) * 1000.0

    counts: Counter[str] = Counter()
    action_ms: Counter[str] = Counter()
    pass_reasons: Counter[str] = Counter()
    pass_reason_ms: Counter[str] = Counter()
    passes: list[dict[str, int | float | str]] = []
    current_pass: dict[str, int | float | str] | None = None

    with args.actions.open(newline="") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            event_id = int(row["event_id"])
            name = row["name"]
            duration_ms = durations.get(event_id, 0.0)

            if "vkCmdDraw" in name:
                counts["draws"] += 1
                action_ms["draw"] += duration_ms
            elif any(token in name for token in ("vkCmdCopy", "vkCmdBlit", "vkCmdResolve")):
                counts["copies"] += 1
                action_ms["copy"] += duration_ms
            elif duration_ms:
                action_ms["other"] += duration_ms
            if "vkQueueSubmit" in name:
                counts["queue_submit_actions"] += 1

            if "vkCmdBeginRenderPass" in name:
                counts["render_passes"] += 1
                current_pass = {"draws": 0, "duration_ms": 0.0, "reason": "unknown"}
            if current_pass is not None:
                current_pass["duration_ms"] = float(current_pass["duration_ms"]) + duration_ms
                if "vkCmdDraw" in name:
                    current_pass["draws"] = int(current_pass["draws"]) + 1
                if name.startswith("cemu.render_pass.end."):
                    current_pass["reason"] = name.removeprefix("cemu.render_pass.end.")
                if "vkCmdEndRenderPass" in name:
                    passes.append(current_pass)
                    current_pass = None

    for render_pass in passes:
        reason = str(render_pass["reason"])
        pass_reasons[reason] += 1
        pass_reason_ms[reason] += float(render_pass["duration_ms"])

    result = {
        "counts": dict(counts),
        "gpu_action_ms": {key: round(value, 6) for key, value in action_ms.items()},
        "gpu_action_total_ms": round(sum(action_ms.values()), 6),
        "render_pass": {
            "reasons": dict(pass_reasons),
            "reason_action_ms": {
                key: round(value, 6) for key, value in pass_reason_ms.most_common()
            },
            "one_draw": sum(int(item["draws"]) == 1 for item in passes),
            "at_most_four_draws": sum(int(item["draws"]) <= 4 for item in passes),
            "zero_draw": sum(int(item["draws"]) == 0 for item in passes),
        },
    }
    if args.pass_state:
        result["pass_state"] = summarize_pass_state(args.pass_state)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
