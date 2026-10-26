"""Android ARM64 Unity player 安装、启动和 acceptance 运行器。"""

from __future__ import annotations

import shutil
import subprocess
import time
import os
from pathlib import Path
from typing import Any

from common import (
    REPOSITORY_ROOT,
    blocked,
    build_native_plugin,
    build_unity_player,
    find_android_ndk,
    find_executable,
    find_unity_editor,
    make_fixture_project,
    make_run_record,
    merge_runtime_result,
    parse_result_marker,
    unity_license_missing,
)


PACKAGE_NAME = "com.il2cppnativebridge.fixture"


def run(entry: dict[str, Any], options: Any) -> dict[str, Any]:
    version = entry["unityVersion"]
    target = "android-arm64-v8a"
    build_root = REPOSITORY_ROOT / "build" / "matrix" / version / target
    adb = find_executable(None, ("ADB",), "adb")
    cmake = find_executable(None, (), "cmake")
    editor = find_unity_editor(version, options.unity_editor)
    ndk = find_android_ndk(options.ndk)
    device = options.device or os.environ.get("ANDROID_SERIAL")
    reasons: list[tuple[str, str]] = []
    if not device:
        reasons.append(("no_device", "Android device serial is missing; pass --device or set ANDROID_SERIAL"))
    if adb is None:
        reasons.append(("toolchain_missing", "adb executable was not found"))
    if cmake is None:
        reasons.append(("toolchain_missing", "CMake executable was not found"))
    if editor is None:
        reasons.append(("no_editor", "Unity Editor executable was not found; set --unity-editor or UNITY_EDITOR"))
    if ndk is None:
        reasons.append(("no_ndk", "Android NDK was not found; set --ndk, ANDROID_NDK_HOME, or ANDROID_NDK_ROOT"))
    if reasons:
        return blocked(entry, target, reasons)

    ready = subprocess.run(
        [adb, "-s", device, "get-state"], capture_output=True, text=True,
        timeout=15, check=False,
    )
    if ready.returncode != 0 or ready.stdout.strip() != "device":
        return blocked(entry, target, [("no_device", f"Android device is not ready: {(ready.stderr or ready.stdout).replace(device, '<redacted>').strip()}")])
    abi = subprocess.run(
        [adb, "-s", device, "shell", "getprop", "ro.product.cpu.abi"],
        capture_output=True, text=True, timeout=15, check=False,
    )
    if abi.returncode != 0 or abi.stdout.strip() != "arm64-v8a":
        return _failed(entry, target, "selected device is not arm64-v8a", abi.stdout + abi.stderr)

    plugin, error, is_blocked = build_native_plugin(
        target, build_root, cmake, ndk, options.android_platform
    )
    if error:
        return blocked(entry, target, [("toolchain_missing", error)]) if is_blocked else _failed(entry, target, "native fixture plugin build failed", error)

    apk_path = build_root / "bridge-fixture.apk"
    project = make_fixture_project(target, version, plugin, build_root)
    try:
        built, log = build_unity_player(editor, project, version, "Android", apk_path, options.timeout)
    finally:
        shutil.rmtree(project, ignore_errors=True)
    if not built:
        if unity_license_missing(log):
            return blocked(entry, target, [("no_editor", "Unity Editor license is inactive; activate it in Unity Hub before testing")])
        return _failed(entry, target, "Unity Android player build failed", log)

    installed = subprocess.run(
        [adb, "-s", device, "install", "-r", str(apk_path)],
        capture_output=True, text=True, timeout=options.timeout, check=False,
    )
    if installed.returncode != 0:
        return _failed(entry, target, "Unity fixture APK installation failed", installed.stdout + installed.stderr)

    stopped = subprocess.run(
        [adb, "-s", device, "shell", "am", "force-stop", PACKAGE_NAME],
        capture_output=True, text=True, timeout=15, check=False,
    )
    if stopped.returncode != 0:
        return _failed(entry, target, "Unity fixture could not be stopped before launch", stopped.stdout + stopped.stderr)
    cleared = subprocess.run(
        [adb, "-s", device, "logcat", "-c"],
        capture_output=True, text=True, timeout=15, check=False,
    )
    if cleared.returncode != 0:
        return _failed(entry, target, "Android logcat could not be cleared before launch", cleared.stdout + cleared.stderr)
    launched = subprocess.run(
        [adb, "-s", device, "shell", "monkey", "-p", PACKAGE_NAME, "1"],
        capture_output=True, text=True, timeout=30, check=False,
    )
    if launched.returncode != 0:
        return _failed(entry, target, "Unity fixture launch failed", launched.stdout + launched.stderr)

    deadline = time.monotonic() + options.timeout
    log_output = ""
    payload = None
    while time.monotonic() < deadline:
        logs = subprocess.run(
            [adb, "-s", device, "logcat", "-d", "-s", "Unity:I", "*:S"],
            capture_output=True, text=True, timeout=20, check=False,
        )
        log_output = logs.stdout + logs.stderr
        payload = parse_result_marker(log_output)
        if payload is not None:
            break
        time.sleep(1)
    subprocess.run([adb, "-s", device, "shell", "am", "force-stop", PACKAGE_NAME],
                   capture_output=True, text=True, timeout=15, check=False)
    if payload is None:
        return _failed(entry, target, "Unity fixture did not emit an acceptance result before timeout", log_output)
    return merge_runtime_result(make_run_record(entry, target), payload)


def _failed(entry: dict[str, Any], target: str, reason: str, details: str) -> dict[str, Any]:
    from common import result_for

    result = result_for(entry, target, "failed", reason)
    result["details"] = f"{reason}: {details[-12000:]}"
    return result
