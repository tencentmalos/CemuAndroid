# BotW Japanese title reference

Use internal metadata, not archive names, to identify this title set:

| Layer | Title ID | Expected version |
| --- | --- | ---: |
| Base | `00050000101c9300` | `v0` |
| Update | `0005000e101c9300` | `v208` |
| DLC | `0005000c101c9300` | `v80` |

The simplified-Chinese patch inspected for this workflow uses an SD Cafiine-style title root and contains these update-layer files:

- `content/Font/Font_jp.sbfarc`
- `content/Pack/Bootup.pack`
- `content/Pack/Bootup_JPja.pack`
- `content/Pack/Title.pack`
- `meta/bootDrcTex.tga`
- `meta/bootTvTex.tga`

The patch's original NAND and USB scripts target the Japanese update install directory (`0005000e/101c9300`). Therefore, bake the title-relative tree into WUA folder `0005000e101c9300_v208`. Match title paths case-insensitively: the patch's `Font_jp.sbfarc` replaces the update file named `Font_JP.sbfarc`. Do not bake it into the base folder and do not enable a duplicate external graphic pack.

The packaging report must show all three internal IDs/versions and an exact six-file overlay verification for this patch layout.
