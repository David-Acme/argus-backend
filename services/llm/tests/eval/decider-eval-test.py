import hashlib
import importlib.util
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

spec = importlib.util.spec_from_file_location("decider_eval", HARNESS)
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)


def case(identifier, text, tools=None, route="none", role="owner", variant="neutral", ambiguous=None):
    calls = [{"tool": tool, "args": []} for tool in (tools or [])]
    expect = {"calls": calls, "allowed": []}
    if ambiguous:
        expect["ambiguous"] = {"tools": ambiguous}
    return {"id": identifier, "group": "none", "lang": "es", "variant": variant, "role": role,
            "modules": ["productivity"], "script": [text], "route": route, "expect": expect}


def write(path, records):
    path.write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in records))


def loaded(records):
    directory = pathlib.Path(tempfile.mkdtemp())
    write(directory / "c.jsonl", records)
    return harness.load_cases([directory / "c.jsonl"])


class OutcomeTest(unittest.TestCase):
    def test_a_confident_clear_answer_acts(self):
        self.assertEqual(harness.outcome(("a", 0.95, "b", 0.1), (0.9, 0.5, 0.2)), ("act", ("a",)))

    def test_the_middle_band_asks_about_the_top_answer(self):
        self.assertEqual(harness.outcome(("a", 0.7, None, 0.0), (0.9, 0.5, 0.2)), ("ask", ("a",)))

    def test_a_small_margin_between_the_top_two_asks_either_or(self):
        self.assertEqual(harness.outcome(("a", 0.95, "b", 0.9), (0.9, 0.5, 0.2)), ("ask", ("a", "b")))

    def test_a_low_answer_stays_conversation(self):
        self.assertEqual(harness.outcome(("a", 0.3, "b", 0.2), (0.9, 0.5, 0.2)), ("none", ()))
        self.assertEqual(harness.outcome((None, 0.0, None, 0.0), (0.9, 0.5, 0.2)), ("none", ()))

    def test_without_an_ask_band_it_is_the_old_threshold(self):
        self.assertEqual(harness.outcome(("a", 0.89, None, 0.0), (0.9, 0.9, 0.0)), ("none", ()))
        self.assertEqual(harness.outcome(("a", 0.9, None, 0.0), (0.9, 0.9, 0.0)), ("act", ("a",)))


class ScoreTest(unittest.TestCase):
    def setUp(self):
        self.cases = loaded([
            case("p1", "agenda una cita", ["calendar.create_event"]),
            case("p2", "crea una tarea", ["task.create"]),
            case("n1", "ayer fui al dentista"),
            case("n2", "la agenda del congreso", variant="neutral"),
            case("a1", "recuérdame o agéndame lo del jueves", ambiguous=["calendar.create_event", "memory.remind"]),
        ])

    def test_a_wrong_act_on_a_negative_is_counted_and_a_wrong_ask_is_not(self):
        decisions = [("calendar.create_event", 0.95, None, 0.0), ("task.create", 0.95, None, 0.0),
                     ("calendar.create_event", 0.95, None, 0.0), ("calendar.create_event", 0.7, None, 0.0),
                     ("calendar.create_event", 0.7, "memory.remind", 0.65)]
        summary = harness.summarise(self.cases, decisions, (0.9, 0.5, 0.0))
        pooled = summary["moduleFamilies"]
        self.assertEqual(pooled["wrongAct"], 1)
        self.assertAlmostEqual(pooled["askRateOther"], 0.5)
        self.assertEqual(pooled["positives"], 2)
        self.assertAlmostEqual(pooled["coverage"], 1.0)
        self.assertEqual(pooled["ambiguous"], 1)
        self.assertAlmostEqual(pooled["ambiguousAskRate"], 1.0)
        self.assertAlmostEqual(pooled["ambiguousActRate"], 0.0)

    def test_acting_on_an_ambiguous_utterance_is_counted(self):
        decisions = [(None, 0.0, None, 0.0)] * 4 + [("calendar.create_event", 0.95, None, 0.0)]
        pooled = harness.summarise(self.cases, decisions, (0.9, 0.5, 0.0))["moduleFamilies"]
        self.assertAlmostEqual(pooled["ambiguousActRate"], 1.0)
        self.assertAlmostEqual(pooled["ambiguousAskRate"], 0.0)

    def test_a_correct_ask_counts_toward_coverage_and_a_wrong_tool_act_does_not(self):
        decisions = [("calendar.create_event", 0.7, None, 0.0), ("task.list", 0.95, None, 0.0),
                     (None, 0.0, None, 0.0), (None, 0.0, None, 0.0), (None, 0.0, None, 0.0)]
        pooled = harness.summarise(self.cases, decisions, (0.9, 0.5, 0.0))["moduleFamilies"]
        self.assertAlmostEqual(pooled["coverage"], 0.5)
        self.assertAlmostEqual(pooled["actCoverage"], 0.0)
        self.assertAlmostEqual(pooled["wrongToolRate"], 0.5)
        self.assertAlmostEqual(pooled["askRateClear"], 0.5)

    def test_the_near_miss_stratum_is_reported_apart_from_real_negatives(self):
        cases = loaded([case("n", "una agenda"), case("r", "frase corriente", variant="real")])
        decisions = [("calendar.create_event", 0.95, None, 0.0), (None, 0.0, None, 0.0)]
        pooled = harness.summarise(cases, decisions, (0.9, 0.9, 0.0))["moduleFamilies"]
        self.assertAlmostEqual(pooled["wrongActRate"], 0.5)
        self.assertAlmostEqual(pooled["authoredWrongActRate"], 1.0)

    def test_easy_real_negatives_cannot_dilute_the_near_miss_stratum(self):
        records = [case("n", "una agenda")] + [case(f"r{i}", f"frase {i}", variant="real") for i in range(2000)]
        cases = loaded(records)
        decisions = [("calendar.create_event", 0.95, None, 0.0)] + [(None, 0.0, None, 0.0)] * 2000
        limits = {"wrongAct": 0.001, "askClear": 0.1, "wrongTool": 0.01}
        pooled = harness.summarise(cases, decisions, (0.9, 0.9, 0.0))["moduleFamilies"]
        self.assertLessEqual(pooled["wrongActRate"], limits["wrongAct"])
        self.assertAlmostEqual(pooled["authoredWrongActRate"], 1.0)
        self.assertIsNone(harness.summarise_pooled(cases, decisions, (0.9, 0.9, 0.0), limits))


class RunTest(unittest.TestCase):
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
        self.write_gates(hashlib.sha256(self.sealed.read_bytes()).hexdigest(), 0.0)

    def write_gates(self, digest, ceiling, metrics=None):
        self.gates.write_text(json.dumps({
            "sealed": {"file": "sealed.jsonl", "sha256": digest},
            "decider": {"wrongActMax": ceiling, "metrics": metrics or {}}}))

    def run_harness(self, *extra):
        report = self.directory / "report.json"
        result = subprocess.run(
            [sys.executable, "-I", str(HARNESS), "--decider", f"{sys.executable} -I {STUB}",
             "--gates", str(self.gates), "--select", str(self.select), "--report", str(report), *extra],
            capture_output=True, text=True, timeout=120)
        data = json.loads(report.read_text()) if report.exists() else {}
        return result, data

    def test_the_policy_is_the_one_with_the_most_coverage_and_no_wrong_act(self):
        result, data = self.run_harness()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(data["policy"]["act"], 0.6)
        self.assertTrue(data["selectionPassed"])
        self.assertEqual(data["selection"]["moduleFamilies"]["wrongAct"], 0)

    def test_a_ceiling_no_policy_can_meet_is_reported(self):
        write(self.select, [case("x", "una certeza absoluta"), case("y", "ya", ["calendar.create_event"])])
        result, data = self.run_harness()
        self.assertIsNone(data["policy"])
        self.assertFalse(data["selectionPassed"])

    def test_the_final_measurement_scores_the_sealed_set_at_the_chosen_policy(self):
        result, data = self.run_harness("--final", "--sealed", str(self.sealed))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(data["sealed"]["moduleFamilies"]["wrongAct"], 0)
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

    def test_a_wrong_act_on_the_sealed_set_fails_the_final_measurement(self):
        write(self.sealed, [case("s1", "agenda para el martes", ["calendar.create_event"]),
                            case("s3", "una certeza que no pide nada")])
        self.write_gates(hashlib.sha256(self.sealed.read_bytes()).hexdigest(), 0.0)
        result, data = self.run_harness("--final", "--sealed", str(self.sealed))
        self.assertEqual(result.returncode, 1)
        self.assertIn("sealed moduleFamilies.wrongActRate", result.stdout)
        self.assertEqual(data["sealed"]["moduleFamilies"]["wrongAct"], 1)

    def test_a_second_sealed_set_is_checked_against_its_own_hash(self):
        second = self.directory / "sealed2.jsonl"
        write(second, [case("t1", "agenda para el jueves", ["calendar.create_event"]), case("t2", "dudoso otra vez")])
        self.gates.write_text(json.dumps({
            "sealed": {"file": "sealed.jsonl", "sha256": hashlib.sha256(self.sealed.read_bytes()).hexdigest()},
            "sealed2": {"file": "sealed2.jsonl", "sha256": "0" * 64},
            "decider": {"wrongActMax": 0.0}}))
        result, _ = self.run_harness("--final", "--sealed", str(self.sealed), "--sealed2", str(second))
        self.assertEqual(result.returncode, 1)
        self.assertIn("the sealed2 set changed", result.stdout)

    def test_a_missing_decider_is_a_visible_skip(self):
        result = subprocess.run(
            [sys.executable, "-I", str(HARNESS), "--decider", "/nonexistent/decider",
             "--gates", str(self.gates), "--select", str(self.select)],
            capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 77)

    def test_real_traffic_reports_how_many_turns_act_ask_or_stay_conversation(self):
        traffic = self.directory / "traffic.tsv"
        traffic.write_text("memory_save\tes\trecuerda que la llave está abajo\nnone\tes\tcuéntame un chiste\n"
                           "none\tes\tuna agenda\nmemory_recall\tes\tdónde está nada\n")
        result, data = self.run_harness("--traffic", str(traffic))
        share = data["traffic"]["traffic.tsv"]
        self.assertEqual(share["turns"], 4)
        self.assertAlmostEqual(share["actShare"], 0.5)
        self.assertAlmostEqual(share["conversationShare"], 0.5)
        self.assertAlmostEqual(share["memoryCoverage"], 0.5)
        self.assertAlmostEqual(share["falseActionRate"], 0.5)

    def test_a_tool_that_was_not_offered_is_counted_as_none(self):
        rogue = self.directory / "rogue.py"
        rogue.write_text("import json, sys\nfor line in sys.stdin:\n    r = json.loads(line)\n"
                         "    print(json.dumps({'seq': r['seq'], 'tool': 'modules.disable', 'confidence': 1.0}), flush=True)\n")
        write(self.select, [case("m", "pide vigilancia al dueño", ["modules.request"], role="resident")])
        report = self.directory / "report.json"
        subprocess.run([sys.executable, "-I", str(HARNESS), "--decider", f"{sys.executable} -I {rogue}",
                        "--gates", str(self.gates), "--select", str(self.select), "--report", str(report)],
                       capture_output=True, text=True, timeout=120)
        data = json.loads(report.read_text())
        self.assertEqual(data["offeredViolations"], 1)

    def test_the_selection_errors_can_be_written_and_the_sealed_set_never_is(self):
        errors = self.directory / "errors.jsonl"
        write(self.select, [case("a", "agenda una cita", ["calendar.create_event"]), case("d", "algo dudoso")])
        result, data = self.run_harness("--errors", str(errors), "--final", "--sealed", str(self.sealed))
        rows = [json.loads(line) for line in errors.read_text().splitlines()]
        self.assertTrue(all(r["set"] == "select" for r in rows))
        self.assertNotIn("buenos días", errors.read_text())

    def test_cached_selection_decisions_are_reused_without_the_decider(self):
        cache = self.directory / "cache.json"
        first, _ = self.run_harness("--cache", str(cache))
        self.assertEqual(first.returncode, 0, first.stdout)
        self.assertTrue(cache.exists())
        report = self.directory / "again.json"
        result = subprocess.run(
            [sys.executable, "-I", str(HARNESS), "--decider", "/nonexistent/decider", "--gates", str(self.gates),
             "--select", str(self.select), "--cache", str(cache), "--report", str(report)],
            capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("latencyMs", json.loads(report.read_text()))

    def test_log_lines_on_the_decider_output_are_ignored(self):
        noisy = self.directory / "noisy.py"
        noisy.write_text("import json, sys\nprint('20261006 INFO model loaded', flush=True)\nfor line in sys.stdin:\n"
                         "    r = json.loads(line)\n    print(json.dumps({'seq': r['seq'], 'tool': None, 'confidence': 0.0}), flush=True)\n")
        report = self.directory / "noisy.json"
        result = subprocess.run([sys.executable, "-I", str(HARNESS), "--decider", f"{sys.executable} -I {noisy}",
                                 "--gates", str(self.gates), "--select", str(self.select), "--report", str(report)],
                                capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_member_is_only_offered_the_request_tool(self):
        self.assertIn("modules.request", harness.offered("resident"))
        self.assertNotIn("modules.enable", harness.offered("resident"))
        self.assertIn("modules.enable", harness.offered("owner"))
        self.assertNotIn("modules.request", harness.offered("owner"))


if __name__ == "__main__":
    unittest.main()
