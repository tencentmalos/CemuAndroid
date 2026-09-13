# macOS Vulkan RenderDoc capture (Cemu / MoltenVK)

Capture a Cemu **Vulkan** frame on macOS so it can be inspected with the
RenderDoc MCP / qrenderdoc. This is the desktop counterpart to
[android-capture.md](android-capture.md); the Android remote-replay flow is
unchanged.

Unlike Android, macOS runs Vulkan through **MoltenVK** (Vulkan-on-Metal). Cemu
normally `dlopen`s `libMoltenVK.dylib` directly, which bypasses the Vulkan
loader and therefore the RenderDoc capture layer. Three things had to change to
make capture possible; all are already in the tree:

1. Cemu no longer hard-links MoltenVK (`src/CMakeLists.txt`) and honors
   `LIBVULKAN_PATH` on macOS (`VulkanAPI.cpp`), so it can load the Vulkan
   **loader** with MoltenVK behind it.
2. The Vulkan instance opts into portability enumeration
   (`VK_KHR_portability_enumeration` + `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`),
   only when the loader advertises it (`VulkanRenderer.cpp`). Without this,
   `vkCreateInstance` returns `-9` (`VK_ERROR_INCOMPATIBLE_DRIVER`) because
   MoltenVK is a non-conformant portability driver.
3. `RenderDocGuestFrameCapture` resolves the in-application API on macOS via
   `RENDERDOC_LIBRARY_PATH` when `RTLD_DEFAULT` does not already have it.

## Prerequisites

- A macOS-native RenderDoc build providing `renderdoccmd` (arm64 Mach-O) and
  `librenderdoc.dylib`. In this environment:
  `~/workspace/renderdoc-for-pico/build-mcp-native/bin/renderdoccmd` and
  `.../lib/librenderdoc.dylib`.
- A Vulkan loader (`libvulkan.*.dylib`) and a MoltenVK ICD manifest. The
  Vulkan SDK provides both, e.g. `~/VulkanSDK/<ver>/macOS/lib/libvulkan.*.dylib`.
- A RelWithDebInfo Cemu desktop build (`bin/Cemu_relwithdebinfo`).

## One-time environment setup

Use the helper to build a **loader-only** directory (a Vulkan loader with **no**
`libMoltenVK.dylib` beside it — leaf-name resolution would otherwise pick the
wrong driver) and a **vklayer** directory containing the RenderDoc capture layer
manifest that points at `librenderdoc.dylib`:

```sh
skills/cemu-renderdoc-analysis/scripts/setup_macos_renderdoc_env.py \
  --vulkan-loader ~/VulkanSDK/1.4.328.1/macOS/lib/libvulkan.1.4.328.dylib \
  --moltenvk /usr/local/lib/libMoltenVK.dylib \
  --renderdoc-library ~/workspace/renderdoc-for-pico/build-mcp-native/lib/librenderdoc.dylib \
  --output-dir _out/renderdoc
```

The ICD's `library_path` **must** point at the exact same MoltenVK that Cemu's
rpath resolves (`/usr/local/lib/libMoltenVK.dylib` here). If the ICD points at a
*different* MoltenVK file, two MoltenVK images load at once, the objc runtime
warns `MVKBlockObserver is implemented in both …`, and MoltenVK deadlocks during
instance creation.

## Launch and capture

```sh
skills/cemu-renderdoc-analysis/scripts/launch_macos_renderdoc.py \
  --renderdoccmd ~/workspace/renderdoc-for-pico/build-mcp-native/bin/renderdoccmd \
  --renderdoc-library ~/workspace/renderdoc-for-pico/build-mcp-native/lib/librenderdoc.dylib \
  --vulkan-loader _out/renderdoc/loader-only/libvulkan.dylib \
  --icd _out/renderdoc/loader-only/MoltenVK_icd.json \
  --expected-moltenvk /usr/local/lib/libMoltenVK.dylib \
  --vk-layer-path _out/renderdoc/vklayer \
  --executable "$(pwd)/bin/Cemu_relwithdebinfo" \
  --working-directory "$(pwd)/bin" \
  --capture-template "$(pwd)/_out/renderdoc/CASE/cemu-frame" \
  --log-path "$(pwd)/_out/renderdoc/CASE/launch.log" \
  --manifest-path "$(pwd)/_out/renderdoc/CASE/launch-manifest.json" \
  -- -g "/ABS/game.wua"
```

Run with `--dry-run` first when changing any path. The helper:

- validates every file identity by SHA-256 and refuses a loader directory that
  contains a competing `libMoltenVK.dylib`;
- launches Cemu under `renderdoccmd capture -w` with the env below and waits for
  the debugbus plus a stability window, then writes a launch manifest.

The env it sets (do not hand-edit; recorded in the manifest):

```text
LIBVULKAN_PATH               -> the loader (Cemu loads Vulkan through it)
VK_ICD_FILENAMES/VK_DRIVER_FILES -> the MoltenVK ICD manifest
RENDERDOC_LIBRARY_PATH       -> librenderdoc.dylib
ENABLE_VULKAN_RENDERDOC_CAPTURE=1
VK_ADD_LAYER_PATH            -> the vklayer directory (loader inserts the layer)
VK_INSTANCE_LAYERS=VK_LAYER_RENDERDOC_Capture
```

Do **not** set `DISABLE_VULKAN_RENDERDOC_CAPTURE_<major>_<minor>` — that variable
turns the capture layer OFF (this RenderDoc is v1.27, so the trap variable is
`DISABLE_VULKAN_RENDERDOC_CAPTURE_1_27`). Do **not** set `DYLD_LIBRARY_PATH` to
the loader directory.

Once Cemu is in a warmed gameplay frame (drive with `warmup_a` and confirm with a
screenshot, per the fixed principles above), trigger a Guest-frame capture over
the debugbus:

```sh
printf 'renderdoc_guest_capture\n' | nc 127.0.0.1 45987
printf 'renderdoc_guest_capture_status\n' | nc 127.0.0.1 45987
```

The `.rdc` is written next to `--capture-template`
(e.g. `_out/renderdoc/CASE/cemu-frame_frameNNNN_0x....rdc`).

## Verifying the hookup

Read the RenderDoc debug log under `/tmp/RenderDocForPico/RenderDoc_*.log`:

- `Registering Vulkan hooks` — the capture layer loaded.
- `Adding Vulkan device frame capturer for 0x…` — the loader inserted the layer
  into Cemu's instance/device chain. If you instead see
  `Couldn't find matching frame capturer … from 0 device frame capturers`, the
  layer was **not** inserted — check `VK_ADD_LAYER_PATH`, the layer JSON's
  `library_path`, and that the disable variable is unset.

Confirm the Cemu child process (the one launched by `renderdoccmd`, not the
wrapper) maps the loader, `librenderdoc.dylib`, and a single `libMoltenVK.dylib`:

```sh
vmmap <cemu-child-pid> | grep -iE 'loader-only/libvulkan|librenderdoc|libMoltenVK'
```

## Replay

macOS can replay a MoltenVK `.rdc` locally through the RenderDoc MCP or
qrenderdoc, subject to the replay driver advertising the same features the
capture used (see [mcp-analysis.md](mcp-analysis.md)). A capture whose pipelines
require a feature the replay driver lacks (e.g. geometry shaders, which Apple
Silicon MoltenVK reports as `geometryShader=false`) may open but fail specific
pipelines on replay; that is a driver-capability boundary, not a corrupt capture.

## Known limitation: geometry-shader titles

Titles that create geometry-shader pipelines (e.g. MH3U's UI) fail those
pipelines on Vulkan/MoltenVK because Apple Silicon MoltenVK reports
`geometryShader=false`. Such a frame never renders those draws, so a Guest-frame
capture of that specific frame reports `state=failed` (no complete frame to
bracket). Capture a title/scene that renders on Vulkan, or use Cemu's Metal
backend (which emulates geometry shaders via Metal mesh shaders) for those
titles — but the Metal backend is not RenderDoc-capturable.
