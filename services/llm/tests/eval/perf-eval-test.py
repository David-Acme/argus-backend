import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
HARNESS = pathlib.Path(os.environ.get("PERF_EVAL", HERE / "perf-eval.py"))
STUB = HERE / "stub-decider.py"


def run(*extra, gates=None):
    directory = pathlib.Path(tempfile.mkdtemp())
    report = directory / "report.json"
    artifact = directory / "model.bin"
    artifact.write_bytes(b"x" * (3 * 1024 * 1024))
    command = [sys.executable, "-I", str(HARNESS), "--decider", f"{sys.executable} -I {STUB}", "--requests", "40",
               "--warmup", "4", "--artifact", str(artifact), "--report", str(report), *extra]
    if gates is not None:
        path = directory / "gates.json"
        path.write_text(json.dumps(gates))
        command += ["--gates", str(path)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    return result, json.loads(report.read_text()) if report.exists() else None


class PerfEvalTest(unittest.TestCase):
    def test_it_measures_latency_memory_threads_cpu_and_size(self):
        result, report = run("--busy-limit", "1.0")
        self.assertEqual(result.returncode, 0, result.stdout)
        metrics = report["metrics"]
        self.assertTrue(metrics["idle"])
        self.assertGreater(metrics["latencyP50Ms"], 0.0)
        self.assertGreaterEqual(metrics["latencyP95Ms"], metrics["latencyP50Ms"])
        self.assertGreater(metrics["residentMb"], 1.0)
        self.assertGreaterEqual(metrics["peakMb"], metrics["residentMb"] - 1.0)
        self.assertGreaterEqual(metrics["threads"], 1)
        self.assertGreaterEqual(metrics["cpuMsPerCall"], 0.0)
        self.assertAlmostEqual(metrics["artifactMb"], 3.0, places=2)

    def test_a_busy_machine_records_nothing(self):
        result, report = run("--busy-limit", "-1")
        self.assertEqual(result.returncode, 77)
        self.assertIsNone(report)
        self.assertIn("no number is recorded", result.stdout)

    def test_a_busy_machine_can_be_measured_but_is_marked_not_reportable(self):
        result, report = run("--busy-limit", "-1", "--allow-busy")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse(report["metrics"]["idle"])
        self.assertIn("NOT REPORTABLE", result.stdout)

    def test_a_budget_the_decider_cannot_meet_fails_the_run(self):
        result, report = run("--busy-limit", "1.0", gates={"performance": {"metrics": {"residentMb": {"max": 0.5}}}})
        self.assertEqual(result.returncode, 1)
        self.assertIn("residentMb", result.stdout)

    def test_a_budget_the_decider_meets_passes(self):
        result, report = run("--busy-limit", "1.0", gates={"performance": {"metrics": {
            "residentMb": {"max": 4096}, "latencyP95Ms": {"max": 5000}, "artifactMb": {"max": 600}}}})
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_a_decider_that_cannot_start_is_a_visible_skip(self):
        result = subprocess.run([sys.executable, "-I", str(HARNESS), "--decider", "/nonexistent/decider",
                                 "--busy-limit", "1.0"], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 77)

    def test_a_decider_that_starts_and_then_exits_fails_the_run_instead_of_hanging(self):
        result = subprocess.run([sys.executable, "-I", str(HARNESS), "--decider", f"{sys.executable} -I -c pass",
                                 "--busy-limit", "1.0"], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 1)
        self.assertIn("closed its output", result.stdout)


if __name__ == "__main__":
    unittest.main()
