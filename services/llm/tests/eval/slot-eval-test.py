import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
HARNESS = pathlib.Path(os.environ.get("SLOT_EVAL", HERE / "slot-eval.py"))
STUB = HERE / "stub-filler.py"
GATES = HERE / "gates.json"
CASES = HERE.parent / "fixtures" / "eval" / "slots.jsonl"

spec = importlib.util.spec_from_file_location("slot_eval", HARNESS)
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)


def make(args=None, missing=None, tool="calendar.create_event"):
    return {"id": "s", "tool": tool, "variant": "neutral", "lang": "es", "user": "x", "now": "2026-10-06T12:30:00-05:00",
            "expect": {"args": args or {}, "missing": missing or []}}


class SlotEvalTest(unittest.TestCase):
    def test_a_time_matches_to_the_minute_whatever_the_offset(self):
        case = make({"starts_at": {"equals": "2026-10-08T15:00:00"}})
        self.assertTrue(harness.score_case(case, {"args": {"starts_at": "2026-10-08T15:00:00-05:00"}})["correct"])
        self.assertTrue(harness.score_case(case, {"args": {"starts_at": "2026-10-08T15:00:00"}})["correct"])
        self.assertFalse(harness.score_case(case, {"args": {"starts_at": "2026-10-08T03:00:00"}})["correct"])
        self.assertFalse(harness.score_case(case, {"args": {}})["correct"])

    def test_a_title_must_hold_its_needles_and_none_of_the_trigger_or_time_words(self):
        case = make({"title": {"contains": ["Andrea"], "notContains": ["jueves"]}})
        self.assertTrue(harness.score_case(case, {"args": {"title": "Reunión con andrea"}})["correct"])
        self.assertFalse(harness.score_case(case, {"args": {"title": "Reunión con Andrea el jueves"}})["correct"])
        self.assertFalse(harness.score_case(case, {"args": {"title": "Reunión"}})["correct"])

    def test_accents_do_not_matter_in_a_title(self):
        case = make({"title": {"contains": ["reunión"]}})
        self.assertTrue(harness.score_case(case, {"args": {"title": "REUNION de padres"}})["correct"])

    def test_a_missing_slot_must_be_asked_for_and_never_guessed(self):
        case = make(missing=["starts_at"])
        self.assertTrue(harness.score_case(case, {"args": {"title": "x"}, "missing": ["starts_at"]})["correct"])
        guessed = harness.score_case(case, {"args": {"starts_at": "2026-10-08T15:00:00"}, "missing": ["starts_at"]})
        self.assertTrue(guessed["guessed"])
        self.assertFalse(guessed["correct"])
        self.assertFalse(harness.score_case(case, {"args": {}, "missing": []})["correct"])

    def test_a_question_nobody_needed_is_counted(self):
        case = make({"title": {"contains": ["a"]}})
        result = harness.score_case(case, {"args": {"title": "a"}, "missing": ["starts_at"]})
        self.assertTrue(result["spuriousClarify"])
        self.assertFalse(result["correct"])

    def test_the_metrics_split_by_argument_and_variant(self):
        results = [harness.score_case(make({"starts_at": {"equals": "2026-10-08T15:00:00"}, "title": {"contains": ["a"]}}),
                                      {"args": {"starts_at": "2026-10-08T15:00:00", "title": "b"}})]
        values = harness.metrics_of(results)
        self.assertEqual(values["timeAccuracy"], 1.0)
        self.assertEqual(values["titleAccuracy"], 0.0)
        self.assertEqual(values["variant.neutral.slotAccuracy"], 0.0)

    def test_the_gates_fail_a_run_that_guesses(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            case = make(missing=["starts_at"])
            case["id"] = "g"
            (root / "cases.jsonl").write_text(json.dumps(case) + "\n")
            (root / "answers.json").write_text(json.dumps({"g": {"args": {"starts_at": "2026-10-08T15:00:00"}, "missing": []}}))
            run = subprocess.run([sys.executable, "-I", str(HARNESS), "--cases", str(root / "cases.jsonl"), "--gates", str(GATES),
                                  "--filler", f"{sys.executable} -I {STUB} {root / 'answers.json'}"],
                                 capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 1, run.stdout)
            self.assertIn("guessedRate", run.stdout)
            (root / "answers.json").write_text(json.dumps({"g": {"args": {}, "missing": ["starts_at"]}}))
            run = subprocess.run([sys.executable, "-I", str(HARNESS), "--cases", str(root / "cases.jsonl"), "--gates", str(GATES),
                                  "--filler", f"{sys.executable} -I {STUB} {root / 'answers.json'}"],
                                 capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout)
            missing = subprocess.run([sys.executable, "-I", str(HARNESS), "--cases", str(root / "cases.jsonl"), "--gates", str(GATES),
                                      "--filler", "/nonexistent/filler"], capture_output=True, text=True, timeout=60)
            self.assertEqual(missing.returncode, 77)


class SlotCorpusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cases = [json.loads(line) for line in CASES.read_text().splitlines() if line.strip()]

    def test_ids_are_unique_and_every_required_slot_is_expected_or_missing(self):
        self.assertEqual(len({c["id"] for c in self.cases}), len(self.cases))
        for case in self.cases:
            self.assertIn(case["tool"], harness.REQUIRED, case["id"])
            wanted = set(case["expect"]["args"]) | set(case["expect"]["missing"])
            for slot in harness.REQUIRED[case["tool"]]:
                self.assertIn(slot, wanted, f"{case['id']} {slot}")

    def test_an_answer_that_follows_the_expectations_scores_perfectly(self):
        results = []
        for case in self.cases:
            args = {}
            for name, matcher in case["expect"]["args"].items():
                args[name] = matcher.get("equals") or matcher.get("startsWith") or " ".join(matcher.get("contains", []))
            results.append(harness.score_case(case, {"args": args, "missing": case["expect"]["missing"]}))
        self.assertTrue(all(r["correct"] for r in results), [r["id"] for r in results if not r["correct"]])

    def test_the_corpus_covers_every_language_variant_and_the_missing_slot_case(self):
        self.assertEqual({c["variant"] for c in self.cases}, {"neutral", "pe", "stt", "en"})
        self.assertGreaterEqual(sum(1 for c in self.cases if c["expect"]["missing"]), 10)
        self.assertGreaterEqual(len(self.cases), 100)


if __name__ == "__main__":
    unittest.main()
