"""Unity Linux x64 IL2CPP 主机端 fixture 运行器。"""

from __future__ import annotations

import platform
import shutil
import subprocess
from pathlib import Path
from typing import Any

from common import (
    REPOSITORY_ROOT,
    blocked,
    build_native_plugin,
    build_unity_player,
    find_executable,
    find_unity_editor,
    host_machine_supported,
    make_fixture_project,
    merge_runtime_result,
    unity_license_missing,
)


def run(entry: dict[str, Any], options: Any) -> dict[str, Any]:
    version = entry["unityVersion"]
    target = "linux-x64"
    build_root = REPOSITORY_ROOT / "build" / "matrix" / version / target
    reasons: list[str] = []
    if not host_machine_supported():
        reasons.append(f"host fixture requires Linux x86_64; current host is {platform.system()} {platform.machine()}")
    editor = find_unity_editor(version, options.unity_editor)
    cmake = find_executable(None, (), "cmake")
    if editor is None:
        reasons.append("Unity Editor executable was not found; set --unity-editor or UNITY_EDITOR")
    if cmake is None:
        reasons.append("CMake executable was not found")
    if reasons:
        return blocked(entry, target, reasons)

    plugin, error, is_blocked = build_native_plugin(target, build_root, cmake, None, options.android_platform)
    if error:
        return blocked(entry, target, [error]) if is_blocked else _failed(entry, target, "native fixture plugin build failed", error)

    player_path = build_root / "player.x86_64"
    project = make_fixture_project(target, version, plugin, build_root)
    try:
        built, log = build_unity_player(
            editor, project, version, "StandaloneLinux64", player_path, options.timeout
        )
    finally:
        shutil.rmtree(project, ignore_errors=True)
    if not built:
        if unity_license_missing(log):
            return blocked(entry, target, ["Unity Editor license is inactive; activate it in Unity Hub before testing"])
        return _failed(entry, target, "Unity player build failed", log)

    try:
        completed = subprocess.run(
            [str(player_path), "-batchmode", "-nographics", "-logFile", "-"],
            capture_output=True,
            text=True,
            timeout=options.timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        return _failed(entry, target, "Unity fixture player timed out", str(error))
    output = completed.stdout + completed.stderr
    if completed.returncode != 0:
        return _failed(entry, target, f"Unity fixture player exited with {completed.returncode}", output)
    payload = _parse_and_merge(entry, target, output)
    if payload is None:
        return _failed(entry, target, "Unity fixture player emitted no valid native result marker", output)
    return payload


def _failed(entry: dict[str, Any], target: str, reason: str, details: str) -> dict[str, Any]:
    from common import result_for

    result = result_for(entry, target, "failed", reason)
    result["details"] = f"{reason}: {details[-12000:]}"
    return result


def _parse_and_merge(entry: dict[str, Any], target: str, output: str) -> dict[str, Any] | None:
    from common import make_run_record, parse_result_marker

    payload = parse_result_marker(output)
    if payload is None:
        return None
    return merge_runtime_result(make_run_record(entry, target), payload)
