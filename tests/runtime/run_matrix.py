#!/usr/bin/env python3
"""按 matrix.json 选择并执行 Unity IL2CPP 兼容性样例。"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from common import load_matrix, make_run_record, result_for, select_entries
import android_arm64_matrix
import host_matrix


MATRIX_PATH = Path(__file__).with_name("matrix.json")


def argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, default=MATRIX_PATH, help="兼容性矩阵 JSON 文件")
    parser.add_argument("--unity-version", help="只运行指定 Unity Editor 版本")
    parser.add_argument("--target", choices=("linux-x64", "android-arm64-v8a"), help="只运行指定目标")
    parser.add_argument("--host-only", action="store_true", help="仅运行 Linux x64 主机样例")
    parser.add_argument("--unity-editor", help="Unity Editor 可执行文件路径；多版本矩阵可用 {version} 模板，也可使用 UNITY_EDITOR")
    parser.add_argument("--device", help="Android 设备序列号；默认读取 ANDROID_SERIAL")
    parser.add_argument("--ndk", help="Android NDK 根目录")
    parser.add_argument("--android-platform", default="android-21", help="Android NDK API level")
    parser.add_argument("--timeout", type=int, default=1800, help="每个 Unity 构建或运行步骤的超时秒数")
    parser.add_argument("--output", type=Path, help="将完整 JSON 结果写入指定路径")
    parser.add_argument("--dry-run", action="store_true", help="只显示选中的版本和目标，不构建或运行")
    return parser


def run_selection(matrix: dict[str, Any], args: argparse.Namespace) -> dict[str, Any]:
    selected = select_entries(matrix, args.unity_version, args.target, args.host_only)
    versions = {entry["unityVersion"] for entry, _ in selected}
    editor_spec = args.unity_editor or os.environ.get("UNITY_EDITOR")
    if not args.dry_run and len(versions) > 1 and (not editor_spec or "{version}" not in editor_spec):
        raise ValueError(
            "a multi-version run requires --unity-editor with a {version} path template; "
            "otherwise select one version with --unity-version"
        )
    runs: list[dict[str, Any]] = []
    for entry, target in selected:
        if args.dry_run:
            record = make_run_record(entry, target)
            record["details"] = "dry-run: fixture selected; runtime was not executed"
        else:
            try:
                if target == "linux-x64":
                    record = host_matrix.run(entry, args)
                else:
                    record = android_arm64_matrix.run(entry, args)
            except subprocess.TimeoutExpired as error:
                record = result_for(entry, target, "failed", f"matrix command timed out: {error}")
            except OSError as error:
                record = result_for(entry, target, "blocked", f"matrix command could not start: {error}")
                record["reason"] = "toolchain_missing"
        runs.append(record)

    statuses = {run["status"] for run in runs}
    if args.dry_run:
        status = "not_run"
    elif "failed" in statuses:
        status = "failed"
    elif "blocked" in statuses:
        status = "blocked"
    elif "not_run" in statuses:
        status = "not_run"
    else:
        status = "passed"
    return {
        "schemaVersion": 1,
        "feature": "il2cpp-native-bridge",
        "generatedAtUtc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "status": status,
        "runs": runs,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argument_parser()
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be a positive number")
    try:
        matrix = load_matrix(args.matrix)
        result = run_selection(matrix, args)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"matrix runner: {error}", file=sys.stderr)
        return 2

    rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return {"passed": 0, "not_run": 0, "blocked": 2, "failed": 1}.get(result["status"], 1)


if __name__ == "__main__":
    raise SystemExit(main())
