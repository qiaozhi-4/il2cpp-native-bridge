"""Android ARM64 Unity player 安装、启动和 acceptance 运行器。"""

from __future__ import annotations

import json
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
    redact,
    unity_license_missing,
)


PACKAGE_NAME = "com.il2cppnativebridge.fixture"
# 验收结果同时落盘：logcat 会截断长行，完整 JSON 只能从文件取。
# Unity 的 persistentDataPath 在 Android 上是外部私有目录，因此两个位置都尝试。
RESULT_FILE_EXTERNAL = "/sdcard/Android/data/" + PACKAGE_NAME + "/files/bridge-result.json"
RESULT_FILE_INTERNAL = "files/bridge-result.json"


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
    # 只要求设备能跑 arm64：部分设备的 ro.product.cpu.abi 报 32 位主 ABI 或 x86_64，
    # 真正的判据是 abilist 里包含 arm64-v8a。
    abi = subprocess.run(
        [adb, "-s", device, "shell", "getprop", "ro.product.cpu.abilist"],
        capture_output=True, text=True, timeout=15, check=False,
    )
    if abi.returncode != 0 or "arm64-v8a" not in abi.stdout:
        return _failed(entry, target, "selected device cannot run arm64-v8a", abi.stdout + abi.stderr)

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
    # 清掉上一次的结果文件，避免读到过期 JSON。
    subprocess.run([adb, "-s", device, "shell", "rm", "-f", RESULT_FILE_EXTERNAL],
                   capture_output=True, text=True, timeout=15, check=False)
    subprocess.run([adb, "-s", device, "shell", "run-as", PACKAGE_NAME, "rm", "-f", RESULT_FILE_INTERNAL],
                   capture_output=True, text=True, timeout=15, check=False)
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
        payload = _read_result_file(adb, device, options.timeout) or parse_result_marker(log_output)
        if payload is not None:
            break
        time.sleep(1)
    subprocess.run([adb, "-s", device, "shell", "am", "force-stop", PACKAGE_NAME],
                   capture_output=True, text=True, timeout=15, check=False)
    if payload is None:
        return _failed(entry, target, "Unity fixture did not emit an acceptance result before timeout", log_output)
    return merge_runtime_result(make_run_record(entry, target), payload)


def _read_result_file(adb: str, device: str, timeout: int) -> dict[str, Any] | None:
    """读取完整验收 JSON：先看外部私有目录，再退回 run-as；文件不存在或未写完时返回 None。"""
    candidates = (
        [adb, "-s", device, "shell", "cat", RESULT_FILE_EXTERNAL],
        [adb, "-s", device, "shell", "run-as", PACKAGE_NAME, "cat", RESULT_FILE_INTERNAL],
    )
    for command in candidates:
        read = subprocess.run(command, capture_output=True, text=True, timeout=timeout, check=False)
        if read.returncode != 0 or not read.stdout.strip():
            continue
        try:
            payload = json.loads(read.stdout)
        except json.JSONDecodeError:
            continue
        if isinstance(payload, dict):
            return payload
    return None


def _failed(entry: dict[str, Any], target: str, reason: str, details: str) -> dict[str, Any]:
    from common import result_for

    result = result_for(entry, target, "failed", reason)
    result["details"] = redact(f"{reason}: {details[-12000:]}")
    return result
