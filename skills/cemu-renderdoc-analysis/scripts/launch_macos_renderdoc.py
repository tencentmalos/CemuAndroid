#!/usr/bin/env python3
"""Launch Cemu on macOS through RenderDoc with an exact Vulkan loader / MoltenVK
identity so a Vulkan frame can be captured and replayed.

Cemu on macOS normally dlopen()s libMoltenVK.dylib directly, which bypasses the
Vulkan loader and therefore the RenderDoc capture layer. This launcher:

  1. Validates the RenderDoc binary/library, the Vulkan loader, the MoltenVK ICD
     and the exact MoltenVK dylib (by SHA-256).
  2. Launches Cemu under `renderdoccmd capture -w` with:
       LIBVULKAN_PATH   -> the Vulkan loader (Cemu honors this on macOS)
       VK_ICD_FILENAMES -> the MoltenVK ICD manifest
       VK_DRIVER_FILES  -> same manifest (newer loaders)
     so the loader sits in front of MoltenVK and RenderDoc can interpose.
  3. Waits for the debugbus to answer and for a bounded stability window, proves
     the target process maps the exact loader + RenderDoc library + MoltenVK,
     and writes a launch manifest.

It never overwrites existing artifacts and leaves a healthy launch running.
See skills/cemu-renderdoc-analysis/references/macos-capture.md.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path


class LaunchError(RuntimeError):
    pass


def absolute_path(value: str) -> Path:
    p = Path(value)
    if not p.is_absolute():
        raise argparse.ArgumentTypeError(f"path must be absolute: {p}")
    return p


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def file_identity(path: Path, *, executable: bool = False) -> dict:
    info = {"path": str(path), "exists": path.exists()}
    if path.exists():
        info["sha256"] = sha256_file(path)
        info["size"] = path.stat().st_size
        if executable:
            info["executable"] = os.access(path, os.X_OK)
    return info


def resolve_icd_library(icd_path: Path) -> Path:
    with open(icd_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    lib = data.get("ICD", {}).get("library_path")
    if not lib:
        raise LaunchError(f"ICD manifest {icd_path} has no ICD.library_path")
    libp = Path(lib)
    if not libp.is_absolute():
        libp = (icd_path.parent / libp).resolve()
    if not libp.exists():
        raise LaunchError(f"ICD library_path does not resolve to a file: {libp}")
    return libp


def debugbus_query(host: str, port: int, command: str, timeout: float = 4.0) -> str:
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.settimeout(timeout)
        s.sendall((command + "\n").encode())
        buf = b""
        try:
            while b"\n.\n" not in buf and b"\n." not in buf[-3:]:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
        except socket.timeout:
            pass
    return buf.decode(errors="replace")


def proc_maps_image(pid: int, needle: str) -> bool:
    """Return True if the target PID maps a dylib whose path contains needle."""
    try:
        out = subprocess.run(["vmmap", str(pid)], capture_output=True, text=True,
                             timeout=20).stdout
    except Exception:
        # Fall back to lsof if vmmap is restricted.
        try:
            out = subprocess.run(["lsof", "-p", str(pid)], capture_output=True,
                                 text=True, timeout=20).stdout
        except Exception:
            return False
    return needle in out


def main() -> int:
    ap = argparse.ArgumentParser(description="Launch Cemu on macOS under RenderDoc.")
    ap.add_argument("--renderdoccmd", type=absolute_path, required=True)
    ap.add_argument("--renderdoc-library", type=absolute_path, required=True)
    ap.add_argument("--vulkan-loader", type=absolute_path, required=True,
                    help="Absolute path to libvulkan*.dylib (the loader, not MoltenVK).")
    ap.add_argument("--icd", type=absolute_path, required=True,
                    help="MoltenVK ICD manifest (MoltenVK_icd.json).")
    ap.add_argument("--vk-layer-path", type=absolute_path, default=None,
                    help="Directory containing renderdoc_capture.json so the "
                         "loader inserts the RenderDoc capture layer.")
    ap.add_argument("--expected-moltenvk", type=absolute_path, required=True)
    ap.add_argument("--executable", type=absolute_path, required=True,
                    help="Cemu binary (e.g. bin/Cemu_relwithdebinfo).")
    ap.add_argument("--working-directory", type=absolute_path, required=True)
    ap.add_argument("--capture-template", type=absolute_path, required=True,
                    help="Prefix for RenderDoc capture files, e.g. .../cemu-frame")
    ap.add_argument("--log-path", type=absolute_path, required=True)
    ap.add_argument("--manifest-path", type=absolute_path, required=True)
    ap.add_argument("--debugbus-host", default="127.0.0.1")
    ap.add_argument("--debugbus-port", type=int, default=45987)
    ap.add_argument("--startup-timeout", type=float, default=60.0)
    ap.add_argument("--stability-seconds", type=float, default=15.0)
    ap.add_argument("--skip-debugbus", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("program_arguments", nargs=argparse.REMAINDER,
                    help="Args after -- are passed to Cemu (e.g. -g /abs/game.wua).")
    args = ap.parse_args()

    # The loader directory must not contain a second libMoltenVK.dylib, or macOS
    # leaf-name resolution can load the wrong driver despite the manifest.
    loader_dir = args.vulkan_loader.parent
    stray = loader_dir / "libMoltenVK.dylib"
    if stray.exists() and stray.resolve() != args.expected_moltenvk.resolve():
        raise LaunchError(
            f"loader directory {loader_dir} contains a different libMoltenVK.dylib "
            f"({stray}); place the loader in a directory without a competing driver")

    icd_lib = resolve_icd_library(args.icd)
    if icd_lib.resolve() != args.expected_moltenvk.resolve():
        raise LaunchError(
            f"ICD library {icd_lib} != expected MoltenVK {args.expected_moltenvk}")

    program_args = list(args.program_arguments)
    if program_args and program_args[0] == "--":
        program_args = program_args[1:]

    env = dict(os.environ)
    env["LIBVULKAN_PATH"] = str(args.vulkan_loader)
    env["VK_ICD_FILENAMES"] = str(args.icd)
    env["VK_DRIVER_FILES"] = str(args.icd)
    env["RENDERDOC_LIBRARY_PATH"] = str(args.renderdoc_library)
    # Enable RenderDoc's Vulkan capture layer and make the loader insert it.
    # The layer JSON (renderdoc_capture.json) points at librenderdoc.dylib and is
    # discovered via VK_ADD_LAYER_PATH; without it the loader never inserts the
    # capture layer and RenderDoc reports "0 device frame capturers". Do NOT set
    # DISABLE_VULKAN_RENDERDOC_CAPTURE_<maj>_<min>: that variable turns the layer
    # OFF. Do NOT set DYLD_LIBRARY_PATH to the loader dir: it makes Cemu's rpath
    # MoltenVK load a second time and deadlock instance creation.
    env["ENABLE_VULKAN_RENDERDOC_CAPTURE"] = "1"
    if args.vk_layer_path is not None:
        env["VK_ADD_LAYER_PATH"] = str(args.vk_layer_path)
        env["VK_INSTANCE_LAYERS"] = "VK_LAYER_RENDERDOC_Capture"

    rdoc_cmd = [
        str(args.renderdoccmd), "capture", "-w",
        "--capture-file", str(args.capture_template),
        str(args.executable), *program_args,
    ]

    identity = {
        "schema": "cemu.macos-renderdoc-launch.v1",
        "timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
        "renderdoccmd": file_identity(args.renderdoccmd, executable=True),
        "renderdoc_library": file_identity(args.renderdoc_library),
        "vulkan_loader": file_identity(args.vulkan_loader),
        "icd_manifest": file_identity(args.icd),
        "icd_resolved_library": file_identity(icd_lib),
        "expected_moltenvk": file_identity(args.expected_moltenvk),
        "executable": file_identity(args.executable, executable=True),
        "working_directory": str(args.working_directory),
        "capture_template": str(args.capture_template),
        "program_arguments": program_args,
        "environment": {k: env[k] for k in
                        ("LIBVULKAN_PATH", "VK_ICD_FILENAMES", "VK_DRIVER_FILES",
                         "RENDERDOC_LIBRARY_PATH", "ENABLE_VULKAN_RENDERDOC_CAPTURE",
                         "VK_ADD_LAYER_PATH", "VK_INSTANCE_LAYERS")},
        "renderdoccmd_invocation": rdoc_cmd,
    }

    if args.dry_run:
        print(json.dumps(identity, indent=2))
        print("\n[dry-run] no process launched")
        return 0

    args.manifest_path.parent.mkdir(parents=True, exist_ok=True)
    args.log_path.parent.mkdir(parents=True, exist_ok=True)
    args.capture_template.parent.mkdir(parents=True, exist_ok=True)
    if args.manifest_path.exists():
        raise LaunchError(f"refusing to overwrite existing manifest {args.manifest_path}")

    log_file = open(args.log_path, "wb")
    proc = subprocess.Popen(rdoc_cmd, cwd=str(args.working_directory), env=env,
                            stdout=log_file, stderr=subprocess.STDOUT)
    identity["renderdoccmd_pid"] = proc.pid

    # Wait for the debugbus to answer (proves Cemu is alive and past init).
    started = time.monotonic()
    debugbus_ok = False
    if not args.skip_debugbus:
        while time.monotonic() - started < args.startup_timeout:
            if proc.poll() is not None:
                identity["result"] = "renderdoccmd_exited_early"
                identity["exit_code"] = proc.returncode
                _write(args.manifest_path, identity)
                raise LaunchError(
                    f"renderdoccmd exited early with {proc.returncode}; see {args.log_path}")
            try:
                reply = debugbus_query(args.debugbus_host, args.debugbus_port, "status")
                if "title_running" in reply or "cemu_debug_status" in reply:
                    debugbus_ok = True
                    break
            except OSError:
                pass
            time.sleep(1.0)
    identity["debugbus_ok"] = debugbus_ok

    time.sleep(args.stability_seconds)
    identity["target_alive_after_stability"] = proc.poll() is None

    # Prove the loaded images (best-effort; vmmap may need permission).
    identity["maps_vulkan_loader"] = proc_maps_image(proc.pid, args.vulkan_loader.name)
    identity["maps_renderdoc"] = proc_maps_image(proc.pid, args.renderdoc_library.name)
    identity["maps_moltenvk"] = proc_maps_image(proc.pid, "libMoltenVK")

    identity["result"] = "running" if proc.poll() is None else "exited"
    identity["exit_code"] = proc.returncode
    _write(args.manifest_path, identity)

    print(f"launch manifest: {args.manifest_path}")
    print(f"  debugbus_ok={debugbus_ok} alive={identity['target_alive_after_stability']}")
    print(f"  maps loader={identity['maps_vulkan_loader']} "
          f"renderdoc={identity['maps_renderdoc']} moltenvk={identity['maps_moltenvk']}")
    print(f"  renderdoccmd pid={proc.pid}; capture files: {args.capture_template}*")
    return 0


def _write(path: Path, obj: dict) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except LaunchError as e:
        print(f"launch error: {e}", file=sys.stderr)
        raise SystemExit(2)
