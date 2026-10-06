import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPORT = pathlib.Path(__file__).resolve().parent / "round-report.py"
FAMILIES = ("calendar", "task", "project", "modules", "reminders")


def entry(**overrides):
    base = {"positives": 100, "coverage": 0.6, "actCoverage": 0.5, "precision": 0.99, "askRateClear": 0.08, "wrongAct": 1,
            "others": 1000, "wrongActRate": 0.001, "wrongActUpper": 0.005, "authoredWrongActRate": 0.0,
            "authoredWrongActUpper": 0.01, "authoredOthers": 300, "askRateOther": 0.01, "ambiguous": 10,
            "ambiguousAskRate": 0.7, "ambiguousActRate": 0.0, "wrongToolRate": 0.002}
    return {**base, **overrides}


def summary(coverage):
    return {"policy": {"act": 0.9, "ask": 0.5, "margin": 0.1, "now": 0.8, "guardMemory": True}, "cases": 5000,
            "moduleFamilies": entry(coverage=coverage), "memory": entry(), "families": {f: entry() for f in FAMILIES},
            "variants": {"es": entry(), "en": entry(), "pe": entry()}}


def report(rows, passed):
    return {"selectionPassed": passed, "priceOfCeiling": rows,
            "traffic": {"production.tsv": {"turns": 61, "actShare": 0.4, "askShare": 0.1, "conversationShare": 0.5,
                                           "falseActionRate": 0.02}},
            "latencyMs": {"p50": 80.0, "p95": 101.0, "n": 200}}


class RoundReportTest(unittest.TestCase):
    def setUp(self):
        self.directory = pathlib.Path(tempfile.mkdtemp())

    def write_round(self, name, rows, passed, calibration=True, perf=None):
        path = self.directory / name
        path.mkdir()
        (path / "calibrated.json").write_text(json.dumps(report(rows, passed)))
        if calibration:
            (path / "calibration.json").write_text(json.dumps({"report": {"confidence": {
                "pairs": 900, "chosen": "platt",
                "heldOutEce": {"before": 0.2, "temperature": 0.05, "platt": 0.03, "isotonic": 0.04}}}}))
        if perf:
            (path / "perf.json").write_text(json.dumps(perf))
        return f"{name}={path}"

    def run_report(self, *rounds, extra=()):
        return subprocess.run([sys.executable, "-I", str(REPORT), *[a for r in rounds for a in ("--round", r)], *extra],
                              capture_output=True, text=True, timeout=60)

    def test_a_round_that_meets_the_gate_is_reported_as_the_gate(self):
        rows = [{"ceiling": 0.001, "policy": {}, "summary": summary(0.62)},
                {"ceiling": 0.01, "policy": {}, "summary": summary(0.7)}]
        result = self.run_report(self.write_round("joint", rows, True))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("at the 0.1% gate", result.stdout)
        self.assertIn("coverage 0.620 (bar 0.456; gate-obeying fastText 0.154 on the selection set at the 0.1% ceiling "
                      "after calibration, 0.101 on the sealed set at confidence >= 0.995 under the older 0.5% gate)",
                      result.stdout)
        self.assertIn("| confidence | 900 | 0.2000 | 0.0500 | 0.0300 | 0.0400 | platt |", result.stdout)
        self.assertIn("| production.tsv | 61 | 40.00% | 10.00% | 50.00% | 2.00% |", result.stdout)

    def test_a_round_that_misses_the_gate_is_labelled_information_at_the_first_feasible_ceiling(self):
        rows = [{"ceiling": 0.001, "policy": None, "summary": None},
                {"ceiling": 0.0025, "policy": None, "summary": None},
                {"ceiling": 0.005, "policy": {}, "summary": summary(0.33)}]
        result = self.run_report(self.write_round("weak", rows, False))
        self.assertIn("the 0.50% ceiling (NOT the gate: information)", result.stdout)
        self.assertIn("| 0.10% | no policy |", result.stdout)

    def test_two_rounds_are_compared_at_the_same_ceiling(self):
        joint = [{"ceiling": 0.001, "policy": {}, "summary": summary(0.62)},
                 {"ceiling": 0.01, "policy": {}, "summary": summary(0.7)}]
        alone = [{"ceiling": 0.001, "policy": None, "summary": None},
                 {"ceiling": 0.01, "policy": {}, "summary": summary(0.66)}]
        result = self.run_report(self.write_round("joint", joint, True), self.write_round("choice-only", alone, False),
                                 extra=("--ceiling", "0.01"))
        self.assertIn("| joint | 0.700 |", result.stdout)
        self.assertIn("| choice-only | 0.660 |", result.stdout)
        self.assertIn("1.00% (information)", result.stdout)
        self.assertIn("The coverage bar is 0.456", result.stdout)
        self.assertIn("beside it, gate-obeying fastText 0.154 on the selection set at the 0.1% ceiling", result.stdout)

    def test_performance_is_reported_with_its_gate_verdict_and_an_unmeasured_round_says_so(self):
        rows = [{"ceiling": 0.001, "policy": {}, "summary": summary(0.62)}]
        perf = {"metrics": {"latencyP50Ms": 80.0, "latencyP95Ms": 101.0, "residentMb": 1730.0, "cpuMsPerCall": 740.0,
                            "idle": True},
                "failures": ["residentMb: 1730.00 above 600"]}
        measured = self.run_report(self.write_round("fp32", rows, True, perf=perf))
        self.assertIn("| resident (MB) | 1730.00 |", measured.stdout)
        self.assertIn("FAILED: residentMb: 1730.00 above 600", measured.stdout)
        unmeasured = self.run_report(self.write_round("later", rows, True))
        self.assertIn("Not measured on an idle machine in this round.", unmeasured.stdout)

    def test_slices_are_tabulated_with_the_warning_that_they_are_an_association(self):
        rows = [{"ceiling": 0.001, "policy": {}, "summary": summary(0.62)}]
        rep = report(rows, True)
        rep["slices"] = {"neighbour": {"CC-BY-SA-4.0": {"cases": 120, "moduleFamilies": entry(positives=40, coverage=0.5),
                                                       "memory": entry(positives=30, coverage=0.7)}}}
        path = self.directory / "sliced"
        path.mkdir()
        (path / "calibrated.json").write_text(json.dumps(rep))
        result = self.run_report(f"sliced={path}")
        self.assertIn("| neighbour / CC-BY-SA-4.0 | 120 | 40 | 0.500 | 0.990 | 1 | 30 | 0.700 | 0.990 | 1 |", result.stdout)
        self.assertIn("not the effect of removing that data", result.stdout)

    def test_a_directory_without_a_report_is_an_error(self):
        empty = self.directory / "empty"
        empty.mkdir()
        result = self.run_report(f"none={empty}")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no calibrated.json or uncalibrated.json", result.stderr)


if __name__ == "__main__":
    unittest.main()
