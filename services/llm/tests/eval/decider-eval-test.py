import ast
import hashlib
import importlib.util
import json
import math
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
        limits = {"wrongAct": 0.0, "askClear": 0.1, "wrongTool": 0.01, "minStratum": 0}
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
        limits = {"wrongAct": 0.5, "askClear": 1.0, "wrongTool": 1.0, "minStratum": 0}
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
        reliability = fitted["report"]["confidence"]["reliability"]
        self.assertEqual(sum(r["count"] for r in reliability["before"]), 400)
        self.assertEqual(sum(r["count"] for r in reliability["after"]), 400)
        gap = lambda rows: sum(r["count"] * abs(r["confidence"] - r["accuracy"]) for r in rows)
        self.assertLess(gap(reliability["after"]), gap(reliability["before"]))


    def test_isotonic_is_reported_but_never_chosen_because_its_plateaus_erase_the_ranking(self):
        records, decisions = [], []
        for index in range(600):
            confidence = 0.5 + 0.5 * index / 600
            records.append(case(f"c{index}", f"agenda algo {index}", ["calendar.create_event"]))
            correct = confidence > 0.8
            decisions.append(("calendar.create_event" if correct else "task.create", confidence, None, 0.0, None))
        fitted = harness.fit_calibration(loaded(records), decisions)
        row = fitted["report"]["confidence"]
        self.assertLess(row["heldOutEce"]["isotonic"], min(row["heldOutEce"]["temperature"], row["heldOutEce"]["platt"]))
        self.assertIn(row["chosen"], ("temperature", "platt", "identity"))
        self.assertNotEqual(fitted["confidence"]["type"], "isotonic")


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
        limits = {"wrongAct": 0.001, "askClear": 0.1, "wrongTool": 0.01, "minStratum": 0}
        pooled = harness.summarise(cases, decisions, (0.9, 0.9, 0.0))["moduleFamilies"]
        self.assertLessEqual(pooled["wrongActRate"], limits["wrongAct"])
        self.assertAlmostEqual(pooled["authoredWrongActRate"], 1.0)
        self.assertIsNone(harness.summarise_pooled(cases, decisions, (0.9, 0.9, 0.0), limits))


def near_misses(count, confident):
    cases = loaded([case(f"n{i}", f"una agenda {i}") for i in range(count)])
    tool = "calendar.create_event"
    decisions = [(tool, 0.95, None, 0.0)] * confident + [(None, 0.0, None, 0.0)] * (count - confident)
    return cases, decisions


class SearchGridTest(unittest.TestCase):
    def setUp(self):
        positives = [case(f"p{i}", f"pon una agenda {i}", ["calendar.create_event"]) for i in range(5)]
        negatives = [case(f"n{i}", f"ayer agendé {i}") for i in range(5)]
        self.cases = loaded(positives + negatives)
        tool = "calendar.create_event"
        self.decisions = [(tool, 0.86, None, 0.0)] * 5 + [(tool, 0.8, None, 0.0)] * 5
        self.limits = {"wrongAct": 0.0, "askClear": 0.1, "wrongTool": 0.01, "minStratum": 0}

    def test_the_search_reaches_an_act_between_the_old_hand_picked_values(self):
        policy = harness.choose_policy(self.cases, self.decisions, self.limits)
        self.assertGreater(policy[0], 0.8)
        self.assertLessEqual(policy[0], 0.86)
        self.assertAlmostEqual(harness.summarise(self.cases, self.decisions, policy)["moduleFamilies"]["coverage"], 1.0)

    def test_every_act_may_have_an_empty_ask_band(self):
        for guard in (False, True):
            candidates = {(p[0], p[1]) for p in harness.policies(guard)}
            self.assertTrue(all((act, act) in candidates for act in harness.SEARCH_ACTS))

    def test_the_search_grid_reaches_the_top_of_a_calibrated_range_and_the_sweep_points(self):
        self.assertLessEqual(min(harness.SEARCH_ACTS), 0.5)
        self.assertGreaterEqual(max(harness.SEARCH_ACTS), 0.99)
        self.assertTrue({0.85, 0.88} <= set(harness.SEARCH_ACTS))
        steps = [b - a for a, b in zip(harness.SEARCH_ACTS[:-2], harness.SEARCH_ACTS[1:-1])]
        self.assertLessEqual(max(steps), 0.0101)


class ConstraintTest(unittest.TestCase):
    limits = {"wrongAct": 0.001, "askClear": 0.1, "wrongTool": 0.01, "minStratum": 300}

    def rows_of(self, cases, decisions):
        return harness.pooled_rows(cases, decisions, (0.9, 0.9, 0.0))

    def test_the_policy_fit_judges_point_rates_and_never_an_upper_bound(self):
        cases, decisions = near_misses(2522, 0)
        rows, pooled = self.rows_of(cases, decisions)
        self.assertGreater(pooled["authoredWrongActUpper"], 0.001)
        self.assertEqual(pooled["authoredWrongActRate"], 0.0)
        self.assertTrue(harness.within(rows, pooled, self.limits))
        self.assertEqual(harness.violations(rows, pooled, self.limits), set())

    def test_no_function_of_the_policy_fit_reads_an_upper_bound(self):
        tree = ast.parse(HARNESS.read_text())
        fit = {"policies", "rank_key", "best_per_limit", "choose_policies", "choose_policy", "pooled_rows", "violations",
               "within", "summarise_pooled", "binding_limits", "relaxed_limits", "tally"}
        found = {node.name for node in ast.walk(tree) if isinstance(node, ast.FunctionDef) and node.name in fit}
        self.assertEqual(found, fit)
        for node in ast.walk(tree):
            if isinstance(node, ast.FunctionDef) and node.name in fit:
                source = ast.get_source_segment(HARNESS.read_text(), node)
                self.assertNotIn("Upper", source, node.name)
                self.assertNotIn("wilson", source, node.name)

    def test_the_wilson_bound_is_computed_only_by_the_report_and_the_final_certification(self):
        tree = ast.parse(HARNESS.read_text())
        callers = {node.name for node in ast.walk(tree) if isinstance(node, ast.FunctionDef)
                   and any(isinstance(call, ast.Call) and isinstance(call.func, ast.Name) and call.func.id == "wilson"
                           for call in ast.walk(node))}
        self.assertEqual(callers, {"digest", "certify"})

    def test_the_final_gate_check_reads_point_rates_and_leaves_the_bound_to_the_certification(self):
        tree = ast.parse(HARNESS.read_text())
        source = next(ast.get_source_segment(HARNESS.read_text(), n) for n in ast.walk(tree)
                      if isinstance(n, ast.FunctionDef) and n.name == "check_gates")
        self.assertNotIn("Upper", source)

    def test_a_stratum_below_the_minimum_cannot_pass_by_having_no_errors(self):
        cases, decisions = near_misses(50, 0)
        rows, pooled = self.rows_of(cases, decisions)
        self.assertEqual(harness.violations(rows, pooled, self.limits), {"thinStratum"})
        self.assertFalse(harness.within(rows, pooled, self.limits))
        self.assertTrue(harness.within(rows, pooled, dict(self.limits, minStratum=50)))

    def test_a_thin_stratum_with_an_error_still_fails_at_a_small_minimum(self):
        cases, decisions = near_misses(50, 1)
        rows, pooled = self.rows_of(cases, decisions)
        self.assertIn("nearMiss", harness.violations(rows, pooled, dict(self.limits, minStratum=50)))

    def test_the_repository_gates_keep_the_minimum_stratum_ten_ceilings_deep(self):
        gates = json.loads((HERE / "gates.json").read_text())["decider"]
        self.assertGreaterEqual(gates["minStratum"], math.ceil(3 / (10 * gates["wrongActMax"])))

    def test_each_relaxation_names_the_constraint_it_lifts(self):
        tool = "calendar.create_event"
        cases = loaded([case(f"p{i}", f"pon una agenda {i}", [tool]) for i in range(4)]
                       + [case(f"n{i}", f"ayer agendé {i}") for i in range(4)])
        decisions = [(tool, 0.8, None, 0.0)] * 4 + [(tool, 0.9, None, 0.0)] * 4
        limits = {"wrongAct": 0.0, "askClear": 0.1, "wrongTool": 0.01, "minStratum": 0}
        found = harness.best_per_limit(cases, decisions, [limits] + harness.binding_limits(limits))
        coverage = {name: harness.summarise(cases, decisions, harness.policy_of(best))["moduleFamilies"]["coverage"]
                    for name, best in zip(("gate", *harness.RELAXATIONS, "secondSignalOff"), found)}
        self.assertEqual(coverage["gate"], 0.0)
        self.assertEqual(coverage["askClear"], 1.0)
        self.assertEqual(coverage["secondSignalOff"], 0.0)
        self.assertEqual(coverage["nearMiss"], 0.0)


class CertificationTest(unittest.TestCase):
    def test_the_error_free_count_is_the_smallest_stratum_whose_upper_bound_meets_the_ceiling(self):
        for ceiling, z in ((0.001, 1.96), (0.001, 1.645), (0.005, 1.96)):
            needed = harness.error_free_needed(ceiling, z)
            self.assertLessEqual(harness.wilson(0, needed, z)[1], ceiling)
            self.assertGreater(harness.wilson(0, needed - 1, z)[1], ceiling)

    def test_the_pooled_near_misses_of_both_sealed_sets_fall_short_at_two_sided_95(self):
        short = {"authoredWrongAct": 0, "authoredOthers": 3674}
        result = harness.certify({"sealed": short}, 0.001, 1.96)
        self.assertFalse(result["passed"])
        self.assertEqual(result["errorFreeNeeded"], 3838)
        self.assertTrue(harness.certify({"sealed": short}, 0.001, 1.645)["passed"])

    def test_the_readings_are_pooled_before_the_bound_is_taken(self):
        half = {"authoredWrongAct": 0, "authoredOthers": 2000}
        self.assertFalse(harness.certify({"sealed": half}, 0.001, 1.96)["passed"])
        self.assertTrue(harness.certify({"sealed": half, "sealed2": half}, 0.001, 1.96)["passed"])

    def test_one_error_in_a_pool_of_thousands_fails_the_ceiling(self):
        reading = {"authoredWrongAct": 1, "authoredOthers": 5000}
        self.assertFalse(harness.certify({"sealed": reading}, 0.001, 1.96)["passed"])

    def test_an_empty_pool_is_never_certified(self):
        self.assertFalse(harness.certify({"sealed": {"authoredWrongAct": 0, "authoredOthers": 0}}, 0.001, 1.96)["passed"])


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


class LatencyTest(unittest.TestCase):
    def test_a_decider_that_closes_its_output_during_the_latency_sample_stops_the_run(self):
        import io
        import types
        decider = harness.Decider("unused")
        decider.process = types.SimpleNamespace(stdin=io.StringIO(), stdout=io.StringIO(""))
        with self.assertRaises(SystemExit):
            decider.latencies(loaded([case("a", "pon una agenda el lunes", ["calendar.create_event"])]))


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
            "decider": {"wrongActMax": ceiling, "minStratum": 0, "metrics": metrics or {}}}))

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
        self.assertEqual(data["policy"]["act"], 0.56)
        self.assertAlmostEqual(data["selection"]["moduleFamilies"]["coverage"], 1.0)
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

    def test_slices_score_the_selection_cases_by_a_label_of_their_text(self):
        slices = self.directory / "slices.json"
        slices.write_text(json.dumps({"origin": {"pon una agenda el lunes": "borrowed", "crea una tarea de pintar": "own",
                                                 "cuéntame un chiste": "own"}}))
        result, data = self.run_harness("--slices", str(slices))
        self.assertEqual(result.returncode, 0, result.stdout)
        rows = data["slices"]["origin"]
        self.assertEqual(rows["borrowed"]["cases"], 1)
        self.assertEqual(rows["own"]["cases"], 2)
        self.assertEqual(rows["borrowed"]["moduleFamilies"]["positives"], 1)
        self.assertAlmostEqual(rows["borrowed"]["moduleFamilies"]["coverage"], 1.0)
        self.assertEqual(rows["own"]["moduleFamilies"]["positives"], 1)
        self.assertIn("origin / borrowed", result.stdout)

    def test_slices_of_a_round_with_no_feasible_policy_use_the_first_feasible_ceiling(self):
        write(self.select, [case("a", "pon una agenda el lunes", ["calendar.create_event"]),
                            case("c", "una certeza absoluta", variant="real")]
              + [case(f"r{i}", f"frase {i}", variant="real") for i in range(99)])
        slices = self.directory / "slices.json"
        slices.write_text(json.dumps({"origin": {"pon una agenda el lunes": "x"}}))
        result, data = self.run_harness("--slices", str(slices))
        self.assertIsNone(data["policy"])
        self.assertAlmostEqual(data["slices"]["origin"]["x"]["moduleFamilies"]["coverage"], 1.0)

    def test_the_gate_obeying_baseline_is_printed_beside_the_bar(self):
        self.gates.write_text(json.dumps({
            "sealed": {"file": "sealed.jsonl", "sha256": hashlib.sha256(self.sealed.read_bytes()).hexdigest()},
            "decider": {"wrongActMax": 0.0, "minStratum": 0, "metrics": {"moduleFamilies.coverage": {"min": 0.456}},
                        "gateObeyingFastText": {
                            "selection": {"coverage": 0.154, "label": "at the 0.1% ceiling after calibration"},
                            "sealed": {"coverage": 0.101, "label": "at confidence >= 0.995 under the older 0.5% gate"}}}}))
        result, _ = self.run_harness("--final", "--sealed", str(self.sealed))
        self.assertIn("against the bar 0.456 (fastText where it broke the wrong-ACT gate) and 0.154 "
                      "(gate-obeying fastText on the selection set at the 0.1% ceiling after calibration)", result.stdout)
        self.assertIn("0.101 (gate-obeying fastText on the sealed set at confidence >= 0.995 under the older 0.5% gate)",
                      result.stdout)

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
            "decider": {"wrongActMax": 0.0, "minStratum": 0}}))
        result, _ = self.run_harness("--final", "--sealed", str(self.sealed), "--sealed2", str(second))
        self.assertEqual(result.returncode, 1)
        self.assertIn("the sealed2 set changed", result.stdout)

    def pooled_gates(self, second, z, pool=("sealed", "sealed2")):
        self.gates.write_text(json.dumps({
            "sealed": {"file": "sealed.jsonl", "sha256": hashlib.sha256(self.sealed.read_bytes()).hexdigest()},
            "sealed2": {"file": "sealed2.jsonl", "sha256": hashlib.sha256(second.read_bytes()).hexdigest()},
            "decider": {"wrongActMax": 0.001, "minStratum": 0, "certification": {"z": z, "pool": list(pool)}}}))

    def second_set(self):
        second = self.directory / "sealed2.jsonl"
        write(second, [case("t1", "agenda para el jueves", ["calendar.create_event"]), case("t2", "dudoso otra vez")])
        return second

    def test_the_binding_table_is_printed_and_reported_row_by_row(self):
        result, data = self.run_harness()
        self.assertIn("which constraint binds", result.stdout)
        self.assertEqual([row["relaxed"] for row in data["binding"]],
                         ["nothing relaxed", *harness.RELAXATIONS, "second signal off"])
        self.assertEqual(data["binding"][0]["policy"], data["policy"])

    def test_the_final_read_certifies_the_pooled_near_misses_with_the_wilson_bound(self):
        second = self.second_set()
        self.pooled_gates(second, 0.01)
        result, data = self.run_harness("--final", "--sealed", str(self.sealed), "--sealed2", str(second))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("CERTIFICATION", result.stdout)
        self.assertEqual(data["certification"]["sets"], ["sealed", "sealed2"])
        self.assertEqual(data["certification"]["errors"], 0)
        self.assertTrue(data["certification"]["passed"])

    def test_the_final_read_refuses_a_certification_the_pooled_count_cannot_reach(self):
        second = self.second_set()
        self.pooled_gates(second, 1.96)
        result, data = self.run_harness("--final", "--sealed", str(self.sealed), "--sealed2", str(second))
        self.assertEqual(result.returncode, 1)
        self.assertIn("certification: the near-miss upper bound", result.stdout)
        self.assertIn("error-free needed", result.stdout)
        self.assertFalse(data["certification"]["passed"])

    def test_a_certification_needs_every_set_it_pools_to_have_been_read(self):
        second = self.second_set()
        self.pooled_gates(second, 0.01)
        result, _ = self.run_harness("--final", "--sealed", str(self.sealed))
        self.assertEqual(result.returncode, 1)
        self.assertIn("not read: sealed2", result.stdout)

    def test_the_selection_fit_does_not_wait_for_the_certification_bound(self):
        self.pooled_gates(self.second_set(), 1.96)
        result, data = self.run_harness()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(data["selectionPassed"])

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
