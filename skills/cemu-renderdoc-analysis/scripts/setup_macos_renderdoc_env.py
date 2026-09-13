#!/usr/bin/env python3
"""Build the loader-only and RenderDoc-layer directories for macOS Vulkan capture.

Creates, under --output-dir:
  loader-only/  a Vulkan loader (copied) with NO libMoltenVK.dylib beside it,
                plus a MoltenVK_icd.json whose library_path is the ABSOLUTE path
                to the exact MoltenVK that Cemu's rpath resolves. Keeping the
                loader in a directory without a competing libMoltenVK.dylib stops
                macOS leaf-name resolution from loading the wrong driver, and the
                absolute ICD path prevents a second MoltenVK image from loading
                (which deadlocks MoltenVK init).
  vklayer/      renderdoc_capture.json (VK_LAYER_RENDERDOC_Capture) whose
                library_path points at the given librenderdoc.dylib, so the
                Vulkan loader inserts the capture layer into Cemu's instance.

See skills/cemu-renderdoc-analysis/references/macos-capture.md.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path


def absolute_path(value: str) -> Path:
    p = Path(value).expanduser()
    if not p.is_absolute():
        raise argparse.ArgumentTypeError(f"path must be absolute: {p}")
    return p


LAYER_MANIFEST = {
    "file_format_version": "1.1.2",
    "layer": {
        "name": "VK_LAYER_RENDERDOC_Capture",
        "type": "GLOBAL",
        "library_path": None,  # filled in
        "api_version": "1.3.131",
        "implementation_version": "27",
        "description": "Debugging capture layer for RenderDoc",
        "functions": {
            "vkGetInstanceProcAddr": "VK_LAYER_RENDERDOC_CaptureGetInstanceProcAddr",
            "vkGetDeviceProcAddr": "VK_LAYER_RENDERDOC_CaptureGetDeviceProcAddr",
            "vkNegotiateLoaderLayerInterfaceVersion": "VK_LAYER_RENDERDOC_CaptureNegotiateLoaderLayerInterfaceVersion",
        },
        "pre_instance_functions": {
            "vkEnumerateInstanceExtensionProperties": "VK_LAYER_RENDERDOC_CaptureEnumerateInstanceExtensionProperties"
        },
        "instance_extensions": [{"name": "VK_EXT_debug_utils", "spec_version": "1"}],
        "device_extensions": [
            {"name": "VK_EXT_debug_marker", "spec_version": "4",
             "entrypoints": ["vkDebugMarkerSetObjectTagEXT", "vkDebugMarkerSetObjectNameEXT",
                             "vkCmdDebugMarkerBeginEXT", "vkCmdDebugMarkerEndEXT",
                             "vkCmdDebugMarkerInsertEXT"]},
            {"name": "VK_EXT_tooling_info", "spec_version": "1",
             "entrypoints": ["vkGetPhysicalDeviceToolPropertiesEXT"]},
        ],
        "enable_environment": {"ENABLE_VULKAN_RENDERDOC_CAPTURE": "1"},
        # This must match the RenderDoc build's version. Setting this variable to
        # "1" at runtime DISABLES the layer, so the launcher never sets it.
        "disable_environment": {"DISABLE_VULKAN_RENDERDOC_CAPTURE_1_27": "1"},
    },
}


def main() -> int:
    ap = argparse.ArgumentParser(description="Set up macOS Vulkan RenderDoc capture dirs.")
    ap.add_argument("--vulkan-loader", type=absolute_path, required=True,
                    help="Path to a libvulkan*.dylib (the loader, not MoltenVK).")
    ap.add_argument("--moltenvk", type=absolute_path, required=True,
                    help="Exact MoltenVK the ICD should point at (Cemu's rpath target, "
                         "e.g. /usr/local/lib/libMoltenVK.dylib).")
    ap.add_argument("--renderdoc-library", type=absolute_path, required=True,
                    help="librenderdoc.dylib (exports the VK_LAYER_RENDERDOC_Capture symbols).")
    ap.add_argument("--output-dir", type=absolute_path, required=True,
                    help="Directory to create loader-only/ and vklayer/ under.")
    ap.add_argument("--layer-disable-var", default="DISABLE_VULKAN_RENDERDOC_CAPTURE_1_27",
                    help="Version-matched disable var for the layer JSON (RenderDoc 1.27 default).")
    args = ap.parse_args()

    for p in (args.vulkan_loader, args.moltenvk, args.renderdoc_library):
        if not p.exists():
            print(f"error: not found: {p}", file=sys.stderr)
            return 2

    loader_dir = args.output_dir / "loader-only"
    layer_dir = args.output_dir / "vklayer"
    loader_dir.mkdir(parents=True, exist_ok=True)
    layer_dir.mkdir(parents=True, exist_ok=True)

    # Copy the loader; create a plain libvulkan.dylib symlink so LIBVULKAN_PATH
    # and the @rpath/libvulkan.1.dylib dependency both resolve inside this dir.
    loader_name = args.vulkan_loader.name
    dst_loader = loader_dir / loader_name
    shutil.copy2(args.vulkan_loader, dst_loader)
    for alias in ("libvulkan.dylib", "libvulkan.1.dylib"):
        link = loader_dir / alias
        if link.exists() or link.is_symlink():
            link.unlink()
        link.symlink_to(loader_name)

    # Refuse to leave a competing MoltenVK beside the loader.
    stray = loader_dir / "libMoltenVK.dylib"
    if stray.exists():
        print(f"error: {stray} exists; loader-only dir must not contain MoltenVK",
              file=sys.stderr)
        return 2

    icd = {
        "file_format_version": "1.0.0",
        "ICD": {
            "library_path": str(args.moltenvk),
            "api_version": "1.4.0",
            "is_portability_driver": True,
        },
    }
    (loader_dir / "MoltenVK_icd.json").write_text(json.dumps(icd, indent=4) + "\n")

    manifest = json.loads(json.dumps(LAYER_MANIFEST))  # deep copy
    manifest["layer"]["library_path"] = str(args.renderdoc_library)
    manifest["layer"]["disable_environment"] = {args.layer_disable_var: "1"}
    (layer_dir / "renderdoc_capture.json").write_text(json.dumps(manifest, indent=2) + "\n")

    print("loader-only:", loader_dir)
    print("  loader :", dst_loader.name, "(+ libvulkan.dylib, libvulkan.1.dylib symlinks)")
    print("  icd    : MoltenVK_icd.json ->", args.moltenvk)
    print("vklayer:", layer_dir)
    print("  layer  : renderdoc_capture.json ->", args.renderdoc_library)
    print("\nPass these to launch_macos_renderdoc.py:")
    print(f"  --vulkan-loader {loader_dir / 'libvulkan.dylib'}")
    print(f"  --icd {loader_dir / 'MoltenVK_icd.json'}")
    print(f"  --expected-moltenvk {args.moltenvk}")
    print(f"  --vk-layer-path {layer_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
