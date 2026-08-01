#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 OUTPUT_DIRECTORY" >&2
  exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
output_dir=$1
rules_source="$repo_root/tools/guest-mods/botw-v208-profiler/rules.txt"
patch_source="$repo_root/references/BotW-BetterVR/resources/BreathOfTheWild_BetterVR/patch_debug_PPC_Profiling.asm"

if [ ! -f "$patch_source" ]; then
  echo "missing BetterVR profiler source; initialize references/BotW-BetterVR first" >&2
  exit 1
fi
if [ -e "$output_dir" ]; then
  echo "refusing to overwrite existing path: $output_dir" >&2
  exit 1
fi

mkdir -p "$output_dir"
cp "$rules_source" "$output_dir/rules.txt"
cp "$patch_source" "$output_dir/patch_guest_profiler.asm"

echo "staged BotW v208 Guest profiler at $output_dir"
echo "source_commit=$(git -C "$repo_root/references/BotW-BetterVR" rev-parse HEAD)"
echo "module_matches=0x6267BFD0"
