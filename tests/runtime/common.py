"""运行 Unity IL2CPP 兼容性矩阵所需的公共帮助函数。"""

from __future__ import annotations

import json
import os
import platform
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Any


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_PROJECT = REPOSITORY_ROOT / "tests" / "fixtures" / "unity" / "BridgeFixture"
CHECKS = (
    "runtime_discovery",
    "class_lookup",
    "method_lookup",
    "overload_selection",
    "field_get_set",
    "method_invoke",
    "set_timeScale_find_call_hook_remove_restore",
)
RESULT_MARKER = "IL2CPP_NATIVE_BRIDGE_RESULT:"
RESULT_STATUSES = {"passed", "failed", "blocked", "not_applicable", "not_run"}


def load_matrix(path: Path) -> dict[str, Any]:
    matrix = json.loads(path.read_text(encoding="utf-8"))
    if matrix.get("schemaVersion") != 1 or not isinstance(matrix.get("entries"), list):
        raise ValueError("unsupported compatibility matrix schema")
    return matrix


def select_entries(
    matrix: dict[str, Any],
    unity_version: str | None,
    target: str | None,
    host_only: bool,
) -> list[tuple[dict[str, Any], str]]:
    if host_only and target not in (None, "linux-x64"):
        raise ValueError("--host-only can only be combined with --target linux-x64")
    selected_target = "linux-x64" if host_only else target
    selected: list[tuple[dict[str, Any], str]] = []
    for entry in matrix["entries"]:
        if unity_version is not None and entry.get("unityVersion") != unity_version:
            continue
        for entry_target in entry.get("targets", []):
            if selected_target is None or entry_target == selected_target:
                selected.append((entry, entry_target))
    if not selected:
        filters = ", ".join(value for value in (unity_version, selected_target) if value) or "the requested selection"
        raise ValueError(f"the compatibility matrix has no fixture for {filters}")
    return selected


def empty_checks(status: str, details: str) -> dict[str, dict[str, str]]:
    if status not in RESULT_STATUSES:
        raise ValueError(f"invalid check status: {status}")
    return {name: {"status": status, "details": details} for name in CHECKS}


def result_for(entry: dict[str, Any], target: str, status: str, details: str) -> dict[str, Any]:
    abi = "arm64-v8a" if target == "android-arm64-v8a" else "x86_64"
    label = entry.get("evidenceLabel", "representative")
    if target == "android-arm64-v8a" and label == "host-only":
        raise ValueError("host-only fixture cannot produce Android ARM64 evidence")
    return {
        "schemaVersion": 1,
        "unityVersion": entry["unityVersion"],
        "target": target,
        "abi": abi,
        "evidenceLabel": label,
        "strippingSettings": "fixture link.xml and direct managed references; disable managed stripping where supported",
        "status": status,
        "checks": empty_checks(status, details),
        "details": details,
    }


def overall_status(checks: dict[str, dict[str, str]]) -> str:
    statuses = {item.get("status") for item in checks.values()}
    unknown = statuses - RESULT_STATUSES
    if unknown:
        return "failed"
    if "failed" in statuses:
        return "failed"
    if "blocked" in statuses:
        return "blocked"
    if "not_run" in statuses:
        return "not_run"
    return "passed"


def merge_runtime_result(result: dict[str, Any], payload: dict[str, Any]) -> dict[str, Any]:
    reported = payload.get("checks")
    if not isinstance(reported, dict):
        result["status"] = "failed"
        result["details"] = "native acceptance output does not contain a checks object"
        result["checks"] = empty_checks("failed", result["details"])
        return result

    merged: dict[str, dict[str, str]] = {}
    for name in CHECKS:
        item = reported.get(name)
        if not isinstance(item, dict) or item.get("status") not in RESULT_STATUSES:
            merged[name] = {"status": "failed", "details": "native acceptance omitted a valid status"}
        else:
            merged[name] = {
                "status": item["status"],
                "details": str(item.get("details", "")),
            }
    result["checks"] = merged
    result["status"] = overall_status(merged)
    result["runtimeUnityVersion"] = payload.get("unityVersion") or "unreported"
    result.pop("details", None)
    return result


def make_run_record(entry: dict[str, Any], target: str) -> dict[str, Any]:
    return result_for(entry, target, "not_run", "runtime execution has not started")


def blocked(entry: dict[str, Any], target: str, reasons: list[str]) -> dict[str, Any]:
    detail = "; ".join(reasons)
    return result_for(entry, target, "blocked", detail)


def find_executable(value: str | None, environment_names: tuple[str, ...], default_name: str) -> str | None:
    candidates = [value] if value else []
    candidates.extend(os.environ.get(name) for name in environment_names)
    candidates.append(default_name)
    for candidate in candidates:
        if not candidate:
            continue
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
        path = Path(candidate).expanduser()
        if path.is_file() and os.access(path, os.X_OK):
            return str(path.resolve())
    return None


def find_unity_editor(version: str, editor_spec: str | None) -> str | None:
    version_key = "UNITY_EDITOR_" + "".join(character if character.isalnum() else "_" for character in version).upper()
    if editor_spec and "{version}" in editor_spec:
        candidate = editor_spec.replace("{version}", version)
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
        path = Path(candidate).expanduser()
        if path.is_file() and os.access(path, os.X_OK):
            return str(path.resolve())
        return find_executable(None, (version_key,), "")
    return find_executable(editor_spec, (version_key, "UNITY_EDITOR"), "Unity")


def find_android_ndk(explicit_path: str | None) -> Path | None:
    candidates = [explicit_path]
    candidates.extend(os.environ.get(name) for name in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "NDK_HOME"))
    sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    if sdk:
        candidates.extend(sorted((Path(sdk) / "ndk").glob("*"), reverse=True))
    for candidate in candidates:
        if not candidate:
            continue
        path = Path(candidate).expanduser()
        if (path / "build" / "cmake" / "android.toolchain.cmake").is_file():
            return path.resolve()
    return None


def build_native_plugin(
    target: str,
    build_root: Path,
    cmake: str,
    ndk_path: Path | None,
    android_platform: str,
) -> tuple[Path | None, str | None, bool]:
    build_dir = build_root / "native"
    command = [
        cmake,
        "-S", str(FIXTURE_PROJECT / "native"),
        "-B", str(build_dir),
        f"-DIL2CPP_NATIVE_BRIDGE_SOURCE_DIR={REPOSITORY_ROOT}",
    ]
    if target == "android-arm64-v8a":
        if ndk_path is None:
            return None, "Android NDK is unavailable", True
        command.extend([
            f"-DCMAKE_TOOLCHAIN_FILE={ndk_path / 'build/cmake/android.toolchain.cmake'}",
            "-DANDROID_ABI=arm64-v8a",
            f"-DANDROID_PLATFORM={android_platform}",
        ])
    configured = subprocess.run(command, capture_output=True, text=True, check=False)
    if configured.returncode != 0:
        return None, configured.stdout + configured.stderr, False
    built = subprocess.run(
        [cmake, "--build", str(build_dir), "--target", "il2cpp_native_bridge_acceptance"],
        capture_output=True,
        text=True,
        check=False,
    )
    if built.returncode != 0:
        return None, built.stdout + built.stderr, False
    plugin = build_dir / "libil2cpp_native_bridge_acceptance.so"
    if not plugin.is_file():
        return None, f"CMake succeeded but the native plugin was not created at {plugin}", False
    return plugin, None, False


def make_fixture_project(target: str, unity_version: str, plugin: Path, scratch_root: Path) -> Path:
    scratch_root.mkdir(parents=True, exist_ok=True)
    project = Path(tempfile.mkdtemp(prefix="unity-fixture-", dir=scratch_root))
    shutil.copytree(FIXTURE_PROJECT, project, dirs_exist_ok=True)
    (project / "ProjectSettings").mkdir(parents=True, exist_ok=True)
    (project / "ProjectSettings" / "ProjectVersion.txt").write_text(
        f"m_EditorVersion: {unity_version}\n", encoding="utf-8"
    )

    if target == "android-arm64-v8a":
        plugin_dir = project / "Assets" / "Plugins" / "Android" / "arm64-v8a"
    else:
        plugin_dir = project / "Assets" / "Plugins" / "x86_64"
    plugin_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(plugin, plugin_dir / plugin.name)
    return project


def build_unity_player(
    editor: str,
    project: Path,
    expected_unity_version: str,
    unity_target: str,
    output: Path,
    timeout_seconds: int,
) -> tuple[bool, str]:
    output.parent.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["IL2CPP_NATIVE_BRIDGE_EXPECTED_VERSION"] = expected_unity_version
    environment["IL2CPP_NATIVE_BRIDGE_BUILD_TARGET"] = unity_target
    environment["IL2CPP_NATIVE_BRIDGE_OUTPUT"] = str(output)
    command = [
        editor,
        "-batchmode",
        "-nographics",
        "-quit",
        "-projectPath", str(project),
        "-executeMethod", "BridgeFixtureBuilder.Build",
        "-logFile", "-",
    ]
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
            env=environment,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        return False, f"Unity Editor build timed out after {timeout_seconds}s: {error.stdout or ''} {error.stderr or ''}"
    log = completed.stdout + completed.stderr
    if completed.returncode != 0:
        return False, log
    if not output.exists():
        return False, f"Unity Editor returned success without creating {output}: {log}"
    return True, log


def parse_result_marker(output: str) -> dict[str, Any] | None:
    for line in reversed(output.splitlines()):
        position = line.find(RESULT_MARKER)
        if position < 0:
            continue
        raw = line[position + len(RESULT_MARKER):].strip()
        try:
            payload = json.loads(raw)
        except json.JSONDecodeError:
            return None
        return payload if isinstance(payload, dict) else None
    return None


def host_machine_supported() -> bool:
    return platform.system() == "Linux" and platform.machine().lower() in {"x86_64", "amd64"}


def unity_license_missing(log: str) -> bool:
    return "No valid Unity Editor license found" in log or "License is not active" in log
