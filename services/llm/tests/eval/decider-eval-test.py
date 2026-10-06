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


class SecondSignalTest(unittest.TestCase):
    def test_a_write_tool_below_the_second_signal_is_asked_not_acted(self):
        decision = ("calendar.create_event", 0.97, None, 0.0, 0.4)
        self.assertEqual(harness.outcome(decision, (0.9, 0.5, 0.0, 0.8)), ("ask", ("calendar.create_event",)))
        self.assertEqual(harness.outcome(decision, (0.9, 0.5, 0.0, 0.0)), ("act", ("calendar.create_event",)))

    def test_a_read_tool_is_never_held_by_the_second_signal(self):
        decision = ("calendar.list_events", 0.97, None, 0.0, 0.1)
        self.assertEqual(harness.outcome(decision, (0.9, 0.5, 0.0, 0.9)), ("act", ("calendar.list_events",)))

    def test_changing_the_guard_mode_is_a_write_and_waits_for_the_second_signal(self):
        decision = ("app.set_guard_mode", 0.97, None, 0.0, 0.3)
        self.assertEqual(harness.outcome(decision, (0.9, 0.5, 0.0, 0.8)), ("ask", ("app.set_guard_mode",)))
        self.assertNotIn("app.set_guard_mode", harness.READ_TOOLS)
        self.assertEqual(harness.outcome(("app.set_guard_mode", 0.97, None, 0.0, 0.95), (0.9, 0.5, 0.0, 0.8)),
                         ("act", ("app.set_guard_mode",)))

    def test_a_write_tool_without_a_second_signal_is_asked_when_the_guard_is_on(self):
        decision = ("task.create", 0.97, None, 0.0, None)
        self.assertEqual(harness.outcome(decision, (0.9, 0.5, 0.0, 0.5)), ("ask", ("task.create",)))

    def test_the_guard_removes_a_wrong_act_the_confidence_alone_cannot(self):
        cases = loaded([case("p", "agenda una cita", ["calendar.create_event"]), case("n", "ayer agendé una cita")])
        decisions = [("calendar.create_event", 0.97, None, 0.0, 0.95), ("calendar.create_event", 0.97, None, 0.0, 0.1)]
        self.assertEqual(harness.summarise(cases, decisions, (0.9, 0.9, 0.0))["moduleFamilies"]["wrongAct"], 1)
        guarded = harness.summarise(cases, decisions, (0.9, 0.9, 0.0, 0.8))["moduleFamilies"]
        self.assertEqual(guarded["wrongAct"], 0)
        self.assertAlmostEqual(guarded["actCoverage"], 1.0)
        limits = {"wrongAct": 0.0, "askClear": 0.1, "wrongTool": 0.01}
        self.assertEqual(harness.choose_policy(cases, decisions, limits)[3] > 0, True)


class GuardScopeTest(unittest.TestCase):
    def setUp(self):
        self.cases = loaded([case("p", "recuerda que la llave está abajo", ["memory.remember"]),
                             case("n", "ayer recordé a mi abuela")])
        self.decisions = [("memory.remember", 0.97, None, 0.0, 0.95), ("memory.remember", 0.97, None, 0.0, 0.1)]

    def test_guarding_memory_writes_removes_a_wrong_memory_act_that_leaving_them_open_keeps(self):
        guarded = harness.summarise(self.cases, self.decisions, (0.9, 0.9, 0.0, 0.8, 1))["memory"]
        open_ = harness.summarise(self.cases, self.decisions, (0.9, 0.9, 0.0, 0.8, 0))["memory"]
        self.assertEqual(guarded["wrongAct"], 0)
        self.assertEqual(open_["wrongAct"], 1)
        self.assertAlmostEqual(guarded["actCoverage"], 1.0)

    def test_the_policy_search_reports_the_best_policy_of_each_scope(self):
        limits = {"wrongAct": 0.5, "askClear": 1.0, "wrongTool": 1.0}
        best = harness.choose_policies(self.cases, self.decisions, limits, (1, 0))
        self.assertEqual(set(best), {1, 0})


class CalibrationHarnessTest(unittest.TestCase):
    def test_a_fitted_calibration_maps_an_overconfident_decider_and_is_applied_to_decisions(self):
        import random
        rng = random.Random(3)
        records, decisions = [], []
        for index in range(400):
            confidence = 0.6 + 0.4 * rng.random()
            correct = rng.random() < confidence ** 5
            records.append(case(f"c{index}", f"agenda algo {index}", ["calendar.create_event"]))
            decisions.append(("calendar.create_event" if correct else "task.create", confidence, None, 0.0, None))
        cases = loaded(records)
        fitted = harness.fit_calibration(cases, decisions)
        self.assertIn(fitted["report"]["confidence"]["chosen"], ("temperature", "platt", "isotonic"))
        row = fitted["report"]["confidence"]["heldOutEce"]
        self.assertLess(min(row["temperature"], row["platt"], row["isotonic"]), row["before"])
        mapped = harness.apply_calibration(decisions, fitted)
        self.assertEqual(len(mapped), len(decisions))
        self.assertNotEqual([m[1] for m in mapped], [d[1] for d in decisions])


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


class CacheTest(unittest.TestCase):
    class Dying:
        def __init__(self):
            self.calls = 0

        def decide_all(self, cases):
            self.calls += 1
            if self.calls > 1:
                raise RuntimeError("the time ran out")
            return [("calendar.create_event", 0.9, None, 0.0, None) for _ in cases]

    def test_a_decider_that_dies_midway_leaves_the_finished_batches_in_the_cache(self):
        cases = loaded([case(f"c{i}", f"frase {i}") for i in range(5)])
        path = pathlib.Path(tempfile.mkdtemp()) / "cache.json"
        original = harness.CACHE_BATCH
        harness.CACHE_BATCH = 2
        try:
            with self.assertRaises(RuntimeError):
                harness.cached_decisions(self.Dying(), cases, str(path))
        finally:
            harness.CACHE_BATCH = original
        self.assertEqual(len(json.loads(path.read_text())), 2)


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

    def test_the_price_of_each_ceiling_is_reported_when_the_gate_cannot_be_met(self):
        write(self.select, [case("a", "pon una agenda el lunes", ["calendar.create_event"]),
                            case("c", "una certeza absoluta", variant="real")]
              + [case(f"r{i}", f"frase {i}", variant="real") for i in range(99)])
        result, data = self.run_harness()
        self.assertIsNone(data["policy"])
        prices = {row["ceiling"]: row for row in data["priceOfCeiling"]}
        self.assertIsNone(prices[0.0]["policy"])
        self.assertIsNone(prices[0.005]["policy"])
        self.assertIsNotNone(prices[0.01]["policy"])
        self.assertAlmostEqual(prices[0.01]["summary"]["moduleFamilies"]["coverage"], 1.0)
        self.assertIn("NOT the gate: information", result.stdout)

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

    def test_real_traffic_is_scored_with_the_same_calibration_as_the_selection_set(self):
        traffic = self.directory / "traffic.tsv"
        traffic.write_text("memory_save\tes\trecuerda que la llave está abajo\nnone\tes\tuna agenda\n")
        flattened = self.directory / "flat.json"
        flattened.write_text(json.dumps({"confidence": {"type": "platt", "scale": 1.0, "shift": -10.0}}))
        _, raw = self.run_harness("--traffic", str(traffic))
        _, calibrated = self.run_harness("--traffic", str(traffic), "--calibration", str(flattened))
        self.assertAlmostEqual(raw["traffic"]["traffic.tsv"]["actShare"], 1.0)
        self.assertAlmostEqual(calibrated["traffic"]["traffic.tsv"]["actShare"], 0.0)

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

    def test_a_calibration_can_be_fitted_written_and_applied_from_the_command_line(self):
        calibration_file = self.directory / "calibration.json"
        write(self.select, [case(f"a{i}", f"pon una agenda {i}", ["calendar.create_event"]) for i in range(30)]
              + [case(f"d{i}", f"algo dudoso {i}") for i in range(30)])
        first, data = self.run_harness("--calibrate-out", str(calibration_file))
        self.assertEqual(first.returncode, 0, first.stdout)
        fitted = json.loads(calibration_file.read_text())
        self.assertIn("confidence", fitted)
        self.assertIn("calibration of confidence", first.stdout)
        second, _ = self.run_harness("--calibration", str(calibration_file))
        self.assertEqual(second.returncode, 0, second.stdout)
        self.assertIn("confidences calibrated", second.stdout)

    def test_a_cache_can_be_filled_in_chunks_and_then_scored_without_the_decider(self):
        cache = self.directory / "chunks.json"
        for index in range(2):
            result, _ = self.run_harness("--cache", str(cache), "--chunk", f"{index}/2")
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn(f"chunk {index}/2", result.stdout)
        stored = json.loads(cache.read_text())
        self.assertEqual(len(stored), 5)
        report = self.directory / "scored.json"
        result = subprocess.run(
            [sys.executable, "-I", str(HARNESS), "--decider", "/nonexistent/decider", "--gates", str(self.gates),
             "--select", str(self.select), "--cache", str(cache), "--report", str(report)],
            capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(json.loads(report.read_text())["selectionPassed"])

    def test_a_chunk_index_outside_the_count_is_refused(self):
        result, _ = self.run_harness("--cache", str(self.directory / "bad.json"), "--chunk", "2/2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("below the count", result.stdout + result.stderr)

    def test_a_member_is_only_offered_the_request_tool(self):
        self.assertIn("modules.request", harness.offered("resident"))
        self.assertNotIn("modules.enable", harness.offered("resident"))
        self.assertIn("modules.enable", harness.offered("owner"))
        self.assertNotIn("modules.request", harness.offered("owner"))


if __name__ == "__main__":
    unittest.main()
