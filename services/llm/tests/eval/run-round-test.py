import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
RUN = HERE / "run-round.py"
STUB = HERE / "stub-decider.py"
GATES = {"decider": {"wrongActMax": 0.0, "metrics": {}}}


def case(identifier, text, tools=None, variant="neutral"):
    calls = [{"tool": tool, "args": []} for tool in (tools or [])]
    return {"id": identifier, "group": "none", "lang": "es", "variant": variant, "role": "owner",
            "modules": ["productivity"], "script": [text], "route": "none", "expect": {"calls": calls, "allowed": []}}


class RunRoundTest(unittest.TestCase):
    def setUp(self):
        self.directory = pathlib.Path(tempfile.mkdtemp())
        self.select = self.directory / "select.jsonl"
        records = [case("a", "pon una agenda el lunes", ["calendar.create_event"]),
                   case("b", "crea una tarea de pintar", ["task.create"]),
                   case("c", "recuerda que la llave está abajo", ["memory.remember"]),
                   case("d", "algo dudoso sobre la semana"), case("e", "cuéntame un chiste")]
        self.select.write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in records))
        self.gates = self.directory / "gates.json"
        self.gates.write_text(json.dumps(GATES))
        self.out = self.directory / "round"
        self.decider = f"{sys.executable} -I {STUB}"

    def run_round(self, *extra, decider=None):
        return subprocess.run([sys.executable, "-I", str(RUN), "--name", "stub", "--out", str(self.out),
                               "--decider", decider or self.decider, "--select", str(self.select),
                               "--gates", str(self.gates), "--chunks", "2", *extra],
                              capture_output=True, text=True, timeout=300)

    def test_the_stages_leave_a_cache_a_calibration_two_reports_and_a_markdown_table(self):
        result = self.run_round("--stages", "fill,score,report")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in ("cache.json", "calibration.json", "uncalibrated.json", "calibrated.json", "uncalibrated.txt",
                     "calibrated.txt", "errors.jsonl", "report.md"):
            self.assertTrue((self.out / name).exists(), name)
        self.assertEqual(len(json.loads((self.out / "cache.json").read_text())), 5)
        self.assertIn("| calendar |", (self.out / "report.md").read_text())
        self.assertIn("chunk 1/2", result.stdout)

    def test_slices_reach_the_calibrated_report_and_the_markdown(self):
        slices = self.directory / "slices.json"
        slices.write_text(json.dumps({"origin": {"pon una agenda el lunes": "borrowed", "cuéntame un chiste": "own"}}))
        result = self.run_round("--stages", "fill,score,report", "--slices", str(slices))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads((self.out / "calibrated.json").read_text())
        self.assertEqual(set(data["slices"]["origin"]), {"borrowed", "own"})
        self.assertIn("| origin / borrowed |", (self.out / "report.md").read_text())

    def test_a_second_run_scores_from_the_cache_without_the_decider(self):
        self.assertEqual(self.run_round("--stages", "fill").returncode, 0)
        again = self.run_round("--stages", "score,report", decider="/nonexistent/decider")
        self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
        self.assertTrue((self.out / "calibrated.json").exists())

    def test_a_chunk_that_runs_out_of_time_says_how_to_continue_and_keeps_the_finished_chunks(self):
        slow = self.directory / "slow.py"
        slow.write_text("import sys, time\nfor line in sys.stdin:\n    time.sleep(30)\n")
        result = self.run_round("--stages", "fill", "--seconds", "1", decider=f"{sys.executable} -I {slow}")
        self.assertEqual(result.returncode, 1)
        self.assertIn("run again with more --chunks", result.stdout)

    def test_a_decider_that_cannot_start_is_a_visible_skip(self):
        result = self.run_round("--stages", "fill", decider="/nonexistent/decider")
        self.assertEqual(result.returncode, 77)

    def test_an_unknown_stage_is_refused(self):
        result = self.run_round("--stages", "fill,bake")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown stage", result.stderr)

    def test_a_runner_prefix_wraps_every_decider_command(self):
        marker = self.directory / "runner.log"
        runner = self.directory / "runner.sh"
        runner.write_text(f"#!/bin/sh\necho run >> {marker}\nexec \"$@\"\n")
        runner.chmod(0o755)
        result = self.run_round("--stages", "fill", "--runner", str(runner))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(marker.read_text().split()), 2)


if __name__ == "__main__":
    unittest.main()
