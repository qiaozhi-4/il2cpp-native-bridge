#!/usr/bin/env python3
"""从矩阵 JSON 汇总出可复算的兼容性统计。

用法：
    python3 tests/runtime/summarize_matrix.py tests/results/*.json
    python3 tests/runtime/summarize_matrix.py --markdown tests/results/android-arm64-matrix.json

输出按 target 分组的状态计数与 blocked 原因分布。`--check` 用于校验一份 markdown 中的
数字是否与 JSON 一致，避免手写汇总与原始记录脱节。
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Any

STATUSES = ("passed", "failed", "blocked", "not_run")
TARGET_TITLES = {
    "android-arm64-v8a": "Android ARM64",
    "linux-x64": "Linux x64 主机",
}


def load_runs(paths: list[Path]) -> dict[str, list[dict[str, Any]]]:
    """按 target 归并所有 JSON 中的 runs。"""
    grouped: dict[str, list[dict[str, Any]]] = {}
    for path in paths:
        payload = json.loads(path.read_text(encoding="utf-8"))
        for run in payload.get("runs", []):
            grouped.setdefault(run["target"], []).append(run)
    return grouped


def reason_of(run: dict[str, Any]) -> str:
    """取 blocked 原因分类。

    新记录由 run_matrix.py 直接写入 reason 字段；早期记录没有该字段，按 details 里的
    固定措辞归类，保证旧证据仍可复算。
    """
    if run.get("reason"):
        return str(run["reason"])
    details = str(run.get("details", ""))
    if "host fixture requires" in details:
        return "host_platform_mismatch"
    if "Unity Editor executable was not found" in details or "license is inactive" in details:
        return "no_editor"
    if "Android device" in details:
        return "no_device"
    if "NDK was not found" in details:
        return "no_ndk"
    if "was not found" in details or "could not start" in details:
        return "toolchain_missing"
    return "unspecified"


def summarize(grouped: dict[str, list[dict[str, Any]]]) -> dict[str, Any]:
    """返回每个 target 的状态计数与 blocked 原因分布。"""
    summary: dict[str, Any] = {}
    for target, runs in sorted(grouped.items()):
        statuses = Counter(run.get("status", "unknown") for run in runs)
        reasons = Counter(reason_of(run) for run in runs if run.get("status") == "blocked")
        check_failures = Counter()
        skipped_checks = Counter()
        for run in runs:
            for name, item in (run.get("checks") or {}).items():
                status = item.get("status")
                if status == "failed":
                    check_failures[name] += 1
                elif status in ("not_applicable", "not_run"):
                    skipped_checks[name] += 1
        summary[target] = {
            "total": len(runs),
            "statuses": {status: statuses.get(status, 0) for status in STATUSES if statuses.get(status, 0)},
            "blockedReasons": dict(sorted(reasons.items())),
            "failedChecks": dict(sorted(check_failures.items())),
            "skippedChecks": dict(sorted(skipped_checks.items())),
            "experiments": dict(sorted(Counter(run.get("experiment", "unspecified") for run in runs).items())),
            "versions": [
                {
                    "unityVersion": run["unityVersion"],
                    "status": run.get("status"),
                    "reason": reason_of(run) if run.get("status") == "blocked" else None,
                }
                for run in sorted(runs, key=lambda item: item["unityVersion"])
            ],
        }
    return summary


def render_markdown(summary: dict[str, Any]) -> str:
    lines: list[str] = []
    for target, data in summary.items():
        title = TARGET_TITLES.get(target, target)
        counts = "、".join(f"`{status}` {count}" for status, count in data["statuses"].items())
        lines.append(f"### {title}（`{target}`）")
        lines.append("")
        lines.append(f"共 {data['total']} 个样例：{counts}。")
        if data["blockedReasons"]:
            reasons = "、".join(f"`{reason}` {count}" for reason, count in data["blockedReasons"].items())
            lines.append("")
            lines.append(f"blocked 原因分布：{reasons}。")
        lines.append("")
        lines.append("| Unity 版本 | 状态 | 原因 |")
        lines.append("|---|---|---|")
        for item in data["versions"]:
            reason = item["reason"] or "—"
            lines.append(f"| `{item['unityVersion']}` | `{item['status']}` | `{reason}` |")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def check_markdown(path: Path, summary: dict[str, Any]) -> list[str]:
    """校验 markdown 中每个 target 的样例数与状态计数是否与 JSON 一致。"""
    text = path.read_text(encoding="utf-8")
    problems: list[str] = []
    for target, data in summary.items():
        title = TARGET_TITLES.get(target, target)
        pattern = re.compile(
            rf"### {re.escape(title)}（`{re.escape(target)}`）\s*\n\s*\n共 (\d+) 个样例：([^。]+)。"
        )
        match = pattern.search(text)
        if not match:
            problems.append(f"{target}: 汇总文档中缺少该目标的统计段落")
            continue
        declared_total = int(match.group(1))
        if declared_total != data["total"]:
            problems.append(f"{target}: 文档写 {declared_total} 个样例，JSON 实际 {data['total']} 个")
        declared = dict(re.findall(r"`(\w+)` (\d+)", match.group(2)))
        for status, count in data["statuses"].items():
            if int(declared.get(status, -1)) != count:
                problems.append(
                    f"{target}: `{status}` 文档写 {declared.get(status, '缺失')}，JSON 实际 {count}"
                )
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+", type=Path, help="矩阵 JSON 文件")
    parser.add_argument("--markdown", action="store_true", help="输出 markdown 而不是 JSON")
    parser.add_argument("--check", type=Path, help="校验指定 markdown 与 JSON 是否一致")
    args = parser.parse_args(argv)

    missing = [str(path) for path in args.paths if not path.is_file()]
    if missing:
        print(f"summarize_matrix: 找不到 {', '.join(missing)}", file=sys.stderr)
        return 2

    summary = summarize(load_runs(args.paths))
    if args.check:
        problems = check_markdown(args.check, summary)
        for problem in problems:
            print(f"不一致: {problem}", file=sys.stderr)
        if problems:
            return 1
        print(f"{args.check} 与 JSON 一致")
        return 0

    if args.markdown:
        print(render_markdown(summary), end="")
    else:
        print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
