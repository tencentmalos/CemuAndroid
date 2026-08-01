---
name: cemu-wua-packaging
description: Inspect encrypted Wii U NUS title folders, validate title IDs and versions, bake localization overlays into an update title, and create a verified Cemu WUA archive. Use for multipart Wii U dumps, title.tmd version checks, base/update/DLC matching, BotW v208 and DLC v80 packaging, Chinese patch integration, or WUA inspection.
---

# Cemu WUA Packaging

Build WUA archives from locally provided Wii U title data without assuming filenames or release labels are correct. Treat internal metadata and post-build byte checks as authoritative.

## Workflow

1. Read every included instruction file before extracting or modifying content. Do not infer localization procedure from archive names.
2. Extract only the first volume of each multipart RAR set. Prefer `unar` for encrypted RAR5 archives; do not extract every part independently.
3. Locate the base, update, and DLC `title.tmd` files. Validate each title directory before packaging:

   ```sh
   scripts/inspect_tmd.py --verify-files /path/to/title.tmd
   ```

4. Check that the title types and IDs form one set. For the verified Japanese BotW layout, read [references/botw-jpn.md](references/botw-jpn.md).
5. When localization must be self-contained, pass its title-relative `content/` and optional `meta/` tree through `--update-overlay`. Bake it into the update title; do not install or enable the same external graphic pack at the same time.
6. Create the archive through the checked wrapper. Always state expected versions when they are known:

   ```sh
   scripts/create_wua.py \
     --cemu /path/to/Cemu \
     --base /path/to/base/title.tmd \
     --update /path/to/update/title.tmd \
     --dlc /path/to/dlc/title.tmd \
     --update-overlay /path/to/patch/title-root \
     --expect-update-version 208 \
     --expect-dlc-version 80 \
     --output /path/to/game.wua
   ```

7. Accept an unsuccessful baseline build as a diagnostic result. The writer uses a temporary file and only moves it into place after verification; never rename a failed temporary file into a `.wua` result.
8. Report the WUA's internal `app.xml` title IDs and versions, overlay file count, output size, and SHA-256. Do not use the output filename as version evidence.

## Verification Rules

- `inspect_tmd.py --verify-files` must report `validation=ok` for every input. This checks `.app` sizes, H3 SHA-1 values where present, and required ticket/certificate files.
- Cemu must reopen the completed WUA and match folder metadata against internal `app.xml` metadata.
- When an update overlay is supplied, every regular overlay file must be present in the update title inside the WUA with identical size and bytes. New update-layer files are valid because an overlay may intentionally override a base-title file.
- Link entries exposed by a NUS FST directory but intentionally unavailable through Cemu's filesystem device are not title files and must not be archived.
- Never keep the same localization patch active both inside the WUA and as an external graphic pack.

## External Overlay Fallback

Use `scripts/install_cemu_graphic_pack.py` only when the user explicitly wants a reversible external overlay instead of a self-contained WUA. It is not the default packaging path.

Do not place game data, tickets, keys, passwords, or localization assets in this skill. Keep only reusable tooling and metadata guidance in the repository.
