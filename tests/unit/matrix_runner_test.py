"""矩阵选择、结果格式和 CLI 参数验证。"""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path


RUNTIME_DIR = Path(__file__).resolve().parents[1] / "runtime"
sys.path.insert(0, str(RUNTIME_DIR))

import common
import run_matrix


class MatrixRunnerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.matrix = common.load_matrix(RUNTIME_DIR / "matrix.json")

    def test_host_only_selects_linux_fixture_and_keeps_evidence_label(self) -> None:
        selected = common.select_entries(self.matrix, "5.3.0f4", None, True)
        self.assertEqual(len(selected), 1)
        entry, target = selected[0]
        self.assertEqual(target, "linux-x64")
        self.assertEqual(entry["evidenceLabel"], "host-only")

    def test_android_selection_excludes_host_only_fixture(self) -> None:
        with self.assertRaisesRegex(ValueError, "no fixture"):
            common.select_entries(self.matrix, "5.3.0f4", "android-arm64-v8a", False)

    def test_host_only_rejects_android_target(self) -> None:
        with self.assertRaisesRegex(ValueError, "only be combined"):
            common.select_entries(self.matrix, None, "android-arm64-v8a", True)

    def test_host_selection_does_not_include_android_runs(self) -> None:
        selected = common.select_entries(self.matrix, "2017.4.40f1", "linux-x64", False)
        self.assertEqual([target for _, target in selected], ["linux-x64"])

    def test_runtime_result_format_requires_every_check(self) -> None:
        entry = self.matrix["entries"][0]
        result = common.make_run_record(entry, "linux-x64")
        omitted = "set_timeScale_find_call_hook_remove_restore"
        payload = {"checks": {name: {"status": "passed"} for name in common.CHECKS if name != omitted}}
        merged = common.merge_runtime_result(result, payload)
        self.assertEqual(merged["status"], "failed")
        self.assertEqual(
            merged["checks"]["set_timeScale_find_call_hook_remove_restore"]["status"],
            "failed",
        )

    def test_not_applicable_host_hook_does_not_fail_overall_status(self) -> None:
        checks = {name: {"status": "passed"} for name in common.CHECKS}
        checks["set_timeScale_find_call_hook_remove_restore"] = {"status": "not_applicable"}
        self.assertEqual(common.overall_status(checks), "passed")

    def test_missing_unity_license_is_detected_as_environment_blocker(self) -> None:
        self.assertTrue(common.unity_license_missing("No valid Unity Editor license found."))

    def test_facade_check_cannot_be_skipped(self) -> None:
        checks = {name: {"status": "passed"} for name in common.CHECKS}
        checks["facade_managed_replacement"] = {"status": "not_applicable"}
        self.assertEqual(common.overall_status(checks), "failed")

    def test_legacy_hook_check_may_be_not_applicable_on_host(self) -> None:
        checks = {name: {"status": "passed"} for name in common.CHECKS}
        checks["set_timeScale_find_call_hook_remove_restore"] = {"status": "not_applicable"}
        self.assertEqual(common.overall_status(checks), "passed")

    def test_runtime_result_keeps_experiment_label(self) -> None:
        entry = next(item for item in self.matrix["entries"]
                     if "android-arm64-v8a" in item.get("targets", []))
        result = common.make_run_record(entry, "android-arm64-v8a")
        payload = {"experiment": "runtime-acceptance",
                   "checks": {name: {"status": "passed"} for name in common.CHECKS}}
        merged = common.merge_runtime_result(result, payload)
        self.assertEqual(merged["experiment"], "runtime-acceptance")

    def test_redaction_removes_device_paths_and_addresses(self) -> None:
        text = common.redact("/data/app/x/lib.so crashed at 0x7f00dead on ABC123", "ABC123")
        self.assertNotIn("ABC123", text)
        self.assertNotIn("0x7f00dead", text)
        self.assertNotIn("/data/app/x", text)

    def test_summary_reports_failed_and_skipped_checks(self) -> None:
        import summarize_matrix

        checks = {name: {"status": "passed"} for name in common.CHECKS}
        checks["class_lookup"] = {"status": "failed"}
        checks["method_lookup"] = {"status": "blocked"}
        run = {"target": "android-arm64-v8a", "unityVersion": "6000.3.10f1",
               "status": "failed", "checks": checks, "experiment": "runtime-acceptance"}
        summary = summarize_matrix.summarize({"android-arm64-v8a": [run]})
        self.assertEqual(summary["android-arm64-v8a"]["failedChecks"], {"class_lookup": 1})
        self.assertEqual(summary["android-arm64-v8a"]["experiments"], {"runtime-acceptance": 1})
        self.assertTrue(common.unity_license_missing("License is not active (com.unity.editor.headless)."))
        self.assertFalse(common.unity_license_missing("Android player build failed: compiler error"))

    def test_multi_version_run_requires_versioned_editor_path(self) -> None:
        args = run_matrix.argument_parser().parse_args(["--target", "linux-x64"])
        with self.assertRaisesRegex(ValueError, "path template"):
            run_matrix.run_selection(self.matrix, args)
        templated = run_matrix.argument_parser().parse_args([
            "--target", "linux-x64", "--unity-editor", "/opt/unity/{version}/Editor/Unity"
        ])
        result = run_matrix.run_selection(self.matrix, templated)
        self.assertEqual(result["status"], "blocked")

    def test_dry_run_cli_emits_structured_selection(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                str(RUNTIME_DIR / "run_matrix.py"),
                "--unity-version", "5.3.0f4",
                "--target", "linux-x64",
                "--host-only",
                "--dry-run",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = json.loads(completed.stdout)
        self.assertEqual(result["status"], "not_run")
        self.assertEqual(len(result["runs"]), 1)
        self.assertEqual(result["runs"][0]["unityVersion"], "5.3.0f4")
        self.assertEqual(result["runs"][0]["abi"], "x86_64")

    def test_invalid_selection_returns_cli_error(self) -> None:
        with self.assertRaises(ValueError):
            common.select_entries(self.matrix, "9.9.9", None, False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
