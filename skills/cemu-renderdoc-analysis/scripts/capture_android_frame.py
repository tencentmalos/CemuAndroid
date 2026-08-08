#!/usr/bin/env python3

"""Capture one warmed Cemu Android frame through RenderDoc for Pico."""

from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import hashlib
import json
import shlex
import subprocess
import sys
import time
from pathlib import Path


GPU_SETTINGS = (
    "enable_gpu_debug_layers",
    "gpu_debug_app",
    "gpu_debug_layer_app",
    "gpu_debug_layers",
    "gpu_debug_layers_gles",
)

RENDERDOC_PROPERTIES = (
    "debug.vulkan.layers",
    "debug.gles.layers",
    "debug.rdoc.IGNORE_LAYERS",
    "debug.rdoc.RENDERDOC_CAPOPTS",
    "debug.rdoc.PICO_HOOK_PROC",
    "debug.vr.profiler",
)


class CaptureError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="ADB device serial")
    parser.add_argument("--package", default="info.cemu.cemu")
    parser.add_argument("--activity", default=".MainActivity")
    parser.add_argument(
        "--renderdoc-root",
        type=Path,
        default=Path.home() / "workspace/renderdoc-for-pico",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="Local evidence directory; defaults below _out/renderdoc",
    )
    parser.add_argument(
        "--warmup-args",
        default="10 15000 5000 250 60000",
        help="Arguments passed to warmup_a",
    )
    parser.add_argument("--warmup-timeout", type=int, default=180)
    parser.add_argument("--capture-timeout", type=int, default=300)
    parser.add_argument("--capture-delay", type=float, default=5.0)
    parser.add_argument(
        "--capture-frame-count",
        type=int,
        default=1,
        help=(
            "Capture consecutive Android present-delimited frames for WSI diagnostics; "
            "this does not guarantee a complete Cemu Guest frame"
        ),
    )
    parser.add_argument(
        "--guest-frame-capture",
        action="store_true",
        help="Anchor capture to one complete Cemu Guest GPU frame via debugbus",
    )
    parser.add_argument("--skip-server-install", action="store_true")
    parser.add_argument("--skip-open-last", action="store_true")
    parser.add_argument("--skip-warmup", action="store_true")
    parser.add_argument(
        "--wait-for-gameplay-confirmation",
        action="store_true",
        help="Pause after the evidence screenshot until an operator confirms gameplay",
    )
    parser.add_argument("--keep-target-running", action="store_true")
    return parser.parse_args()


class AndroidCapture:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.repo_root = Path(__file__).resolve().parents[3]
        timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.output_dir = args.output_dir or self.repo_root / "_out/renderdoc" / timestamp
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.device_capture_dir = (
            f"/sdcard/Android/media/{args.package}/files/RenderDocForPico"
        )
        self.server_package = "com.picoxr.renderdoccmd.arm64"
        self.settings_before: dict[str, str] = {}
        self.properties_before: dict[str, str] = {}
        self.attached = False
        self.target_launched = False
        self.manifest: dict[str, object] = {
            "device_serial": args.serial,
            "package": args.package,
            "activity": args.activity,
            "warmup_args": args.warmup_args,
            "capture_frame_count": args.capture_frame_count,
            "guest_frame_capture": args.guest_frame_capture,
            "started_at": dt.datetime.now().astimezone().isoformat(),
        }

    def adb(
        self,
        *arguments: str,
        check: bool = True,
        timeout: float | None = 30,
    ) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            ["adb", "-s", self.args.serial, *arguments],
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        if check and result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise CaptureError(f"adb {' '.join(arguments)} failed: {detail}")
        return result

    def validate(self) -> None:
        if self.args.capture_frame_count < 1:
            raise CaptureError("--capture-frame-count must be at least 1")
        if self.args.guest_frame_capture and self.args.capture_frame_count != 1:
            raise CaptureError(
                "--guest-frame-capture requires --capture-frame-count 1"
            )

        if self.adb("get-state").stdout.strip() != "device":
            raise CaptureError(f"device {self.args.serial} is not online")

        package_dump = self.adb("shell", "dumpsys", "package", self.args.package).stdout
        if "DEBUGGABLE" not in package_dump:
            raise CaptureError(
                f"{self.args.package} is not debuggable; install RelWithDebInfo first"
            )

        library = self.args.renderdoc_root / "build-mcp-native/lib/librenderdoc.dylib"
        apk = (
            self.args.renderdoc_root
            / "build-cemu-android-arm64/bin/com.picoxr.renderdoccmd.arm64.apk"
        )
        if not library.is_file():
            raise CaptureError(f"missing host RenderDoc library: {library}")
        if not apk.is_file():
            raise CaptureError(f"missing Android RenderDoc server: {apk}")

        self.library_path = library
        self.server_apk = apk

    def snapshot_environment(self) -> None:
        for key in GPU_SETTINGS:
            self.settings_before[key] = self.adb(
                "shell", "settings", "get", "global", key
            ).stdout.strip()
        for key in RENDERDOC_PROPERTIES:
            self.properties_before[key] = self.adb("shell", "getprop", key).stdout.strip()
        self.manifest["gpu_settings_before"] = self.settings_before
        self.manifest["renderdoc_properties_before"] = self.properties_before

    def restore_environment(self) -> None:
        for key, value in self.settings_before.items():
            if value == "null":
                self.adb(
                    "shell", "settings", "delete", "global", key, check=False
                )
            else:
                self.adb(
                    "shell", "settings", "put", "global", key, value, check=False
                )
        for key, value in self.properties_before.items():
            command = f"setprop {shlex.quote(key)} {shlex.quote(value)}"
            self.adb("shell", command, check=False)

    def install_server(self) -> None:
        if self.args.skip_server_install:
            return
        result = self.adb(
            "install",
            "--abi",
            "arm64-v8a",
            "-r",
            "-g",
            "--force-queryable",
            str(self.server_apk),
            timeout=120,
        )
        if "Success" not in result.stdout:
            raise CaptureError(f"RenderDoc server install failed: {result.stdout.strip()}")

    def load_api(self) -> None:
        api = ctypes.CDLL(str(self.library_path))
        api.RENDERDOC_AttachRenderdoc.argtypes = [ctypes.c_char_p]
        api.RENDERDOC_AttachRenderdoc.restype = ctypes.c_uint32
        api.RENDERDOC_LaunchTargetAndHook.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        api.RENDERDOC_LaunchTargetAndHook.restype = ctypes.c_uint32
        api.RENDERDOC_CaptureFrame.argtypes = [ctypes.c_char_p]
        api.RENDERDOC_CaptureFrame.restype = ctypes.c_uint32
        self.capture_frames_api = getattr(api, "RENDERDOC_CaptureFrames", None)
        if self.capture_frames_api is not None:
            self.capture_frames_api.argtypes = [ctypes.c_char_p, ctypes.c_int32]
            self.capture_frames_api.restype = ctypes.c_uint32
        api.RENDERDOC_GetTargetIdent.argtypes = [ctypes.c_char_p]
        api.RENDERDOC_GetTargetIdent.restype = ctypes.c_int32
        api.RENDERDOC_DetachRenderdoc.argtypes = [ctypes.c_char_p]
        self.api = api
        self.device_id = self.args.serial.encode()

    def attach_and_launch(self) -> None:
        attach_result = int(self.api.RENDERDOC_AttachRenderdoc(self.device_id))
        self.manifest["attach_result"] = attach_result
        if attach_result != 0:
            raise CaptureError(f"RENDERDOC_AttachRenderdoc failed: ResultCode={attach_result}")
        self.attached = True

        component = f"{self.args.package}/{self.args.activity}".encode()
        launch_result = int(
            self.api.RENDERDOC_LaunchTargetAndHook(self.device_id, component)
        )
        self.manifest["launch_result"] = launch_result
        if launch_result != 0:
            raise CaptureError(
                f"RENDERDOC_LaunchTargetAndHook failed: ResultCode={launch_result}"
            )
        self.target_launched = True
        self.manifest["target_ident"] = int(
            self.api.RENDERDOC_GetTargetIdent(self.device_id)
        )

    def debugbus(self, command: str) -> str:
        component = f"{self.args.package}/.utils.DebugDumpService"
        result = self.adb(
            "shell",
            "dumpsys",
            "activity",
            "service",
            component,
            *command.split(),
            timeout=20,
        )
        return result.stdout.strip()

    def enter_game_and_warmup(self) -> None:
        time.sleep(3)
        if not self.args.skip_open_last:
            response = self.debugbus("open_last_game")
            self.manifest["open_last_game"] = response
            print(response, flush=True)
            if "open_last_game launched" not in response:
                raise CaptureError("open_last_game did not launch the recorded title")

        if self.args.skip_warmup:
            return

        command = "warmup_a"
        if self.args.warmup_args.strip():
            command += " " + self.args.warmup_args.strip()
        response = self.debugbus(command)
        self.manifest["warmup_start"] = response
        print(response, flush=True)

        deadline = time.monotonic() + self.args.warmup_timeout
        last_status = ""
        while time.monotonic() < deadline:
            status = self.debugbus("warmup_status")
            if status != last_status:
                print(status, flush=True)
                last_status = status
            if "warmup_state=completed" in status:
                self.manifest["warmup_status"] = status
                return
            if "warmup_state=failed" in status or "warmup_state=cancelled" in status:
                raise CaptureError(f"warmup did not complete: {status}")
            time.sleep(5)
        raise CaptureError(f"warmup timed out; last status: {last_status}")

    def save_gameplay_screenshot(self) -> None:
        screenshot = self.output_dir / "gameplay-before-capture.png"
        with screenshot.open("wb") as output:
            result = subprocess.run(
                ["adb", "-s", self.args.serial, "exec-out", "screencap", "-p"],
                check=False,
                stdout=output,
                stderr=subprocess.PIPE,
                timeout=30,
            )
        if result.returncode != 0:
            raise CaptureError(f"screencap failed: {result.stderr.decode().strip()}")
        self.manifest["gameplay_screenshot"] = str(screenshot)

    def list_device_captures(self) -> dict[str, tuple[int, int]]:
        find_result = self.adb(
            "shell",
            "find",
            self.device_capture_dir,
            "-type",
            "f",
            "-name",
            "*.rdc",
            check=False,
        )
        captures: dict[str, tuple[int, int]] = {}
        for path in find_result.stdout.splitlines():
            path = path.strip()
            if not path:
                continue
            stat_command = (
                f"stat -c {shlex.quote('%s|%Y')} {shlex.quote(path)}"
            )
            stat = self.adb(
                "shell", stat_command, check=False
            ).stdout.strip()
            try:
                size_text, mtime_text = stat.split("|", 1)
                captures[path] = (int(size_text), int(mtime_text))
            except ValueError:
                continue
        return captures

    def capture_and_pull(self) -> list[Path]:
        before = self.list_device_captures()
        time.sleep(self.args.capture_delay)
        if self.args.guest_frame_capture:
            response = self.debugbus("renderdoc_guest_capture")
            self.manifest["guest_capture_request"] = response
            if "scheduled" not in response:
                raise CaptureError(
                    f"Guest-frame RenderDoc capture was not scheduled: {response}"
                )

            status_deadline = time.monotonic() + self.args.capture_timeout
            last_status = ""
            while time.monotonic() < status_deadline:
                status = self.debugbus("renderdoc_guest_capture_status")
                if status != last_status:
                    print(status, flush=True)
                    last_status = status
                if "renderdoc_guest_capture_state=completed" in status:
                    self.manifest["guest_capture_status"] = status
                    break
                if (
                    "renderdoc_guest_capture_state=failed" in status
                    or "renderdoc_guest_capture_state=unavailable" in status
                ):
                    raise CaptureError(
                        f"Guest-frame RenderDoc capture failed: {status}"
                    )
                time.sleep(1)
            else:
                raise CaptureError(
                    f"Guest-frame RenderDoc capture timed out: {last_status}"
                )
            capture_result = 0
        elif self.args.capture_frame_count == 1:
            capture_result = int(self.api.RENDERDOC_CaptureFrame(self.device_id))
        else:
            if self.capture_frames_api is None:
                raise CaptureError(
                    "host RenderDoc library does not export RENDERDOC_CaptureFrames"
                )
            capture_result = int(
                self.capture_frames_api(
                    self.device_id, self.args.capture_frame_count
                )
            )
        self.manifest["capture_result"] = capture_result
        if capture_result != 0:
            raise CaptureError(f"RENDERDOC_CaptureFrame failed: ResultCode={capture_result}")

        deadline = time.monotonic() + self.args.capture_timeout
        previous: dict[str, tuple[int, int]] = {}
        stable_counts: dict[str, int] = {}
        stable: dict[str, tuple[int, int]] = {}
        while time.monotonic() < deadline:
            current = {
                path: metadata
                for path, metadata in self.list_device_captures().items()
                if path not in before
            }
            for path, metadata in current.items():
                if metadata[0] > 0 and previous.get(path) == metadata:
                    stable_counts[path] = stable_counts.get(path, 0) + 1
                else:
                    stable_counts[path] = 0
                if stable_counts[path] >= 2:
                    stable[path] = metadata
            if len(stable) >= self.args.capture_frame_count:
                break
            previous = current
            time.sleep(3)

        if len(stable) < self.args.capture_frame_count:
            raise CaptureError(
                "capture timed out before all requested RDCs became stable on the device"
            )

        pulled: list[Path] = []
        local_metadata: list[dict[str, object]] = []
        for device_path in sorted(stable):
            local_path = self.output_dir / Path(device_path).name
            self.adb("pull", device_path, str(local_path), timeout=300)
            pulled.append(local_path)
            digest = hashlib.sha256()
            with local_path.open("rb") as capture_file:
                for chunk in iter(lambda: capture_file.read(1024 * 1024), b""):
                    digest.update(chunk)
            local_metadata.append(
                {
                    "path": str(local_path),
                    "device_path": device_path,
                    "size_bytes": local_path.stat().st_size,
                    "sha256": digest.hexdigest(),
                }
            )
        self.manifest["device_captures"] = stable
        self.manifest["local_captures"] = [str(path) for path in pulled]
        self.manifest["capture_files"] = local_metadata
        return pulled

    def cleanup(self) -> None:
        if self.attached:
            self.api.RENDERDOC_DetachRenderdoc(self.device_id)
            self.attached = False
        self.restore_environment()
        self.adb("shell", "am", "force-stop", self.server_package, check=False)
        if self.target_launched and not self.args.keep_target_running:
            self.adb("shell", "am", "force-stop", self.args.package, check=False)
            self.adb(
                "shell",
                "am",
                "start",
                "-n",
                f"{self.args.package}/{self.args.activity}",
                check=False,
            )

    def write_manifest(self, error: str | None = None) -> None:
        self.manifest["finished_at"] = dt.datetime.now().astimezone().isoformat()
        if error:
            self.manifest["error"] = error
        path = self.output_dir / "capture-manifest.json"
        path.write_text(json.dumps(self.manifest, indent=2, ensure_ascii=False) + "\n")

    def run(self) -> list[Path]:
        error: str | None = None
        captures: list[Path] = []
        try:
            self.validate()
            self.snapshot_environment()
            self.install_server()
            self.load_api()
            self.attach_and_launch()
            self.enter_game_and_warmup()
            self.save_gameplay_screenshot()
            if self.args.wait_for_gameplay_confirmation:
                print(
                    f"inspect {self.manifest['gameplay_screenshot']} and press Enter "
                    "only after confirming interactive gameplay",
                    flush=True,
                )
                input()
            captures = self.capture_and_pull()
            return captures
        except BaseException as exc:
            error = str(exc)
            raise
        finally:
            try:
                if hasattr(self, "api"):
                    self.cleanup()
            finally:
                self.write_manifest(error)


def main() -> int:
    args = parse_args()
    capture = AndroidCapture(args)
    try:
        captures = capture.run()
    except Exception as exc:
        print(f"capture failed: {exc}", file=sys.stderr)
        print(f"evidence: {capture.output_dir}", file=sys.stderr)
        return 1

    for path in captures:
        print(f"capture: {path}")
    print(f"evidence: {capture.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
