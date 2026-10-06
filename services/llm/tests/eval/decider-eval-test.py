import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
HARNESS = pathlib.Path(os.environ.get("DECIDER_EVAL", HERE / "decider-eval.py"))
STUB = HERE / "stub-decider.py"


def case(identifier, text, tools=None, route="none", role="owner", variant="neutral"):
    calls = [{"tool": tool, "args": []} for tool in (tools or [])]
    return {"id": identifier, "group": "none", "lang": "es", "variant": variant, "role": role,
            "modules": ["productivity"], "script": [text], "route": route,
            "expect": {"calls": calls, "allowed": []}}


def write(path, records):
    path.write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in records))


class DeciderEvalTest(unittest.TestCase):
    def setUp(self):
        self.directory = pathlib.Path(tempfile.mkdtemp())
        self.select = self.directory / "select.jsonl"
        write(self.select, [
            case("a", "pon una agenda el lunes", ["calendar.create_event"]),
            case("b", "crea una tarea de pintar", ["task.create"]),
            case("c", "recuerda que la llave está abajo", ["memory.remember"]),
            case("d", "algo dudoso sobre la semana"),
            case("e", "cuéntame un chiste"),
        ])
        self.sealed = self.directory / "sealed.jsonl"
        write(self.sealed, [
            case("s1", "agenda para el martes", ["calendar.create_event"]),
            case("s2", "una tarea para mañana", ["task.create"]),
            case("s3", "dudoso, hablemos de planes"),
            case("s4", "buenos días", variant="real"),
        ])
        self.gates = self.directory / "gates.json"
        self.write_gates(hashlib.sha256(self.sealed.read_bytes()).hexdigest(), 0.3)

    def write_gates(self, digest, ceiling, metrics=None):
        self.gates.write_text(json.dumps({
            "sealed": {"file": "sealed.jsonl", "sha256": digest, "falseRouteMax": ceiling},
            "router": {"metrics": metrics or {}}}))

    def run_harness(self, *extra):
        report = self.directory / "report.json"
        result = subprocess.run(
            [sys.executable, "-I", str(HARNESS), "--decider", f"{sys.executable} -I {STUB}",
             "--gates", str(self.gates), "--select", str(self.select), "--report", str(report), *extra],
            capture_output=True, text=True, timeout=120)
        data = json.loads(report.read_text()) if report.exists() else {}
        return result, data

    def test_the_operating_point_is_the_lowest_threshold_that_meets_the_ceiling(self):
        self.write_gates(hashlib.sha256(self.sealed.read_bytes()).hexdigest(), 0.0)
        result, data = self.run_harness()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(data["threshold"], 0.6)
        self.assertTrue(data["selectionPassed"])

    def test_a_ceiling_no_threshold_can_meet_is_reported(self):
        self.write_gates(hashlib.sha256(self.sealed.read_bytes()).hexdigest(), 0.0)
        write(self.select, [case("x", "algo dudoso", []), case("y", "otro dudoso", [])] * 3
              + [case("z", "ya", ["calendar.create_event"])])
        result, data = self.run_harness()
        self.assertEqual(data["threshold"], 0.6)
        write(self.select, [case("x", "una certeza absoluta", [])])
        result, data = self.run_harness()
        self.assertIsNone(data["threshold"])
        self.assertFalse(data["selectionPassed"])

    def test_easy_real_negatives_cannot_dilute_the_near_miss_stratum(self):
        self.write_gates(hashlib.sha256(self.sealed.read_bytes()).hexdigest(), 0.01)
        write(self.select, [case("n", "algo dudoso", [])]
              + [case(f"r{i}", f"frase corriente {i}", [], variant="real") for i in range(200)]
              + [case("z", "una agenda", ["calendar.create_event"])])
        result, data = self.run_harness()
        self.assertEqual(data["threshold"], 0.6)

    def test_the_final_measurement_scores_the_sealed_set_at_the_chosen_threshold(self):
        self.write_gates(hashlib.sha256(self.sealed.read_bytes()).hexdigest(), 0.0)
        result, data = self.run_harness("--final", "--sealed", str(self.sealed))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(data["sealed"]["moduleFamilies"]["falseRoute"], 0)
        self.assertAlmostEqual(data["sealed"]["moduleFamilies"]["coverage"], 1.0)
        self.assertIn("FINAL MEASUREMENT", result.stdout)

    def test_a_changed_sealed_set_is_refused(self):
        self.write_gates("0" * 64, 0.0)
        result, _ = self.run_harness("--final", "--sealed", str(self.sealed))
        self.assertEqual(result.returncode, 1)
        self.assertIn("the sealed set changed", result.stdout)

    def test_a_gate_on_coverage_fails_the_final_measurement(self):
        self.write_gates(hashlib.sha256(self.sealed.read_bytes()).hexdigest(), 0.0,
                         {"moduleFamilies.coverage": {"min": 1.1}})
        result, _ = self.run_harness("--final", "--sealed", str(self.sealed))
        self.assertEqual(result.returncode, 1)
        self.assertIn("moduleFamilies.coverage", result.stdout)

    def test_a_missing_decider_is_a_visible_skip(self):
        result = subprocess.run(
            [sys.executable, "-I", str(HARNESS), "--decider", "/nonexistent/decider",
             "--gates", str(self.gates), "--select", str(self.select)],
            capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 77)

    def test_a_member_is_only_offered_the_request_tool(self):
        sys.path.insert(0, str(HERE))
        import importlib.util
        spec = importlib.util.spec_from_file_location("decider_eval", HARNESS)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        member = module.offered("resident")
        self.assertIn("modules.request", member)
        self.assertNotIn("modules.enable", member)
        owner = module.offered("owner")
        self.assertIn("modules.enable", owner)
        self.assertNotIn("modules.request", owner)


if __name__ == "__main__":
    unittest.main()
