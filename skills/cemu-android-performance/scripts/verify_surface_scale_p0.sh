#!/usr/bin/env bash

set -euo pipefail

adb_args=()
if [[ $# -gt 1 ]]; then
	printf 'usage: %s [device-serial]\n' "$0" >&2
	exit 2
fi
if [[ $# -eq 1 ]]; then
	adb_args=(-s "$1")
fi

service=info.cemu.cemu/.utils.DebugDumpService
dump_command()
{
	adb "${adb_args[@]}" shell dumpsys activity service "$service" "$@"
}

status=$(dump_command status)
surface_status=$(dump_command surface_scale_status)
transitions=$(dump_command surface_scale_transitions 64)
graphic_pack=$(dump_command surface_scale_graphic_pack_conflicts 64)

printf '%s\n%s\n' "$status" "$surface_status"
sed -n '1,/transition_end/p' <<<"$transitions"
sed -n '1,/conflict_end/p' <<<"$graphic_pack"

rg -q 'title_running=true' <<<"$status"
rg -q 'configured_factor=1' <<<"$surface_status"
rg -q 'active_factor=1' <<<"$surface_status"
rg -q 'families_scaled=0' <<<"$surface_status"
rg -q 'tv_guest_extent=1280x720x1' <<<"$surface_status"
rg -q 'tv_host_extent=1280x720x1' <<<"$surface_status"

title_generation=$(awk -F= '/title_generation=/{print $2; exit}' <<<"$surface_status")
copy_conflicts=$(awk -F= '/copy_scale_conflicts=/{print $2; exit}' <<<"$surface_status")
resized_readbacks=$(awk -F= '/resized_readbacks=/{print $2; exit}' <<<"$surface_status")
readback_failures=$(awk -F= '/readback_failures=/{print $2; exit}' <<<"$surface_status")
if [[ -z "$title_generation" || "$title_generation" -le 0 ]]; then
	printf 'P0 verification failed: title-scoped diagnostics were not initialized\n' >&2
	exit 1
fi
if [[ "$copy_conflicts" != 0 || "$resized_readbacks" != 0 || "$readback_failures" != 0 ]]; then
	printf 'P0 verification failed: 1x copy/readback baseline regressed\n' >&2
	exit 1
fi

sampled_to_rt=$(awk -F= '/sampled_to_render_target_count=/{print $2; exit}' <<<"$transitions")
readbacks=$(awk -F= '/readbacks_total=/{print $2; exit}' <<<"$surface_status")
alias_conflicts=$(awk -F= '/alias_conflicts=/{print $2; exit}' <<<"$surface_status")
if [[ -z "$sampled_to_rt" || "$sampled_to_rt" -le 0 ]]; then
	printf 'P0 verification failed: no SampledToRenderTarget transition observed\n' >&2
	exit 1
fi
if [[ -z "$readbacks" || "$readbacks" -le 0 ]]; then
	printf 'P0 verification failed: no readback observed\n' >&2
	exit 1
fi
if [[ -z "$alias_conflicts" || "$alias_conflicts" -le 0 ]]; then
	printf 'P0 verification failed: no alias/reinterpret conflict observed\n' >&2
	exit 1
fi

if [[ "${REQUIRE_GRAPHIC_PACK:-0}" == 1 ]]; then
	fixed_surfaces=$(awk -F= '/graphic_pack_fixed_surfaces=/{print $2; exit}' <<<"$graphic_pack")
	pack_conflicts=$(awk -F= '/graphic_pack_conflict_count=/{print $2; exit}' <<<"$graphic_pack")
	if [[ -z "$fixed_surfaces" || "$fixed_surfaces" -le 0 ]]; then
		printf 'P0 verification failed: Graphic Pack fixture produced no fixed surface\n' >&2
		exit 1
	fi
	if [[ -z "$pack_conflicts" || "$pack_conflicts" -le 0 ]]; then
		printf 'P0 verification failed: Graphic Pack fixture produced no conflict evidence\n' >&2
		exit 1
	fi
fi

printf 'surface scale P0 verification passed\n'
