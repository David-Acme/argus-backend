import json
import os
import pathlib
import subprocess
import tempfile
import unittest

EVAL = os.environ.get("CONTEXT_SCENARIOS_EVAL", "")
GATES = os.environ.get("CONTEXT_SCENARIOS_GATES", "")
SMOKE_GATES = os.environ.get("CONTEXT_SCENARIOS_SMOKE_GATES", "")
MISSING_MODEL = os.environ.get("CONTEXT_SCENARIOS_MISSING_MODEL", "")
CASES = os.environ.get("CONTEXT_SCENARIOS_CASES", "").split(":") if os.environ.get("CONTEXT_SCENARIOS_CASES") else []


def run(gates, arguments, cases=None):
    packs = cases if cases is not None else CASES
    command = [EVAL, "--gates", gates, "--llm-model", MISSING_MODEL]
    for path in packs:
        command.extend(["--cases", path])
    return subprocess.run([*command, *arguments], capture_output=True, text=True, timeout=120)


def loadPacks():
    cases = []
    for path in CASES:
        with open(path, encoding="utf-8") as handle:
            cases.extend(json.loads(line) for line in handle if line.strip())
    return cases


@unittest.skipUnless(EVAL and pathlib.Path(EVAL).exists(), "the context-scenarios-eval binary is not built")
class ContextScenariosPinTest(unittest.TestCase):
    def test_a_run_that_does_not_ask_for_the_pinned_cell_is_refused(self):
        for arguments in (["--temperature", "0.45", "--seed", "42", "--force"],
                          ["--temperature", "0.3", "--seed", "7", "--force"],
                          ["--force"]):
            with self.subTest(arguments=arguments):
                process = run(GATES, arguments)
                self.assertEqual(process.returncode, 77)
                self.assertIn("gates pinned for different args", process.stdout)
                self.assertNotIn("no LLM weights", process.stdout)

    def test_the_pinned_cell_passes_the_args_check_and_stops_at_the_missing_weights(self):
        process = run(GATES, ["--temperature", "0.3", "--seed", "42", "--force"])
        self.assertEqual(process.returncode, 77)
        self.assertNotIn("gates pinned for different args", process.stdout)
        self.assertIn("no LLM weights", process.stdout)

    def test_the_smoke_pin_carries_its_own_temperature_and_seed(self):
        refused = run(SMOKE_GATES, ["--temperature", "0.6", "--seed", "42", "--force"])
        self.assertEqual(refused.returncode, 77)
        self.assertIn("gates pinned for different args", refused.stdout)
        accepted = run(SMOKE_GATES, ["--temperature", "0.3", "--seed", "42", "--force"])
        self.assertEqual(accepted.returncode, 77)
        self.assertIn("no LLM weights", accepted.stdout)

    def test_a_pin_without_its_args_is_an_error(self):
        with tempfile.TemporaryDirectory() as scratch:
            pinless = pathlib.Path(scratch) / "gates-pinless.json"
            pinless.write_text('{"contextScenarios": {"metrics": {"cases": {"min": 1}}}}', encoding="utf-8")
            process = run(str(pinless), ["--temperature", "0.3", "--seed", "42", "--force"])
            self.assertEqual(process.returncode, 1)
            self.assertIn("without the temperature and seed", process.stdout)

    def test_an_unknown_context_variant_and_an_empty_pack_are_errors(self):
        unknown = run(GATES, ["--force", "--variant", "larger"])
        self.assertEqual(unknown.returncode, 1)
        self.assertIn("unknown context variant", unknown.stdout)
        empty = run(GATES, ["--force"], cases=[])
        self.assertEqual(empty.returncode, 1)
        self.assertIn("no scenario pack given", empty.stdout)

    def test_the_pack_holds_the_scenarios_the_plan_names(self):
        cases = loadPacks()
        self.assertEqual(len(cases), 63)
        self.assertEqual(len({case["id"] for case in cases}), 63)
        scenarios = {case["scenario"] for case in cases}
        self.assertLessEqual({"panic", "guard-episode", "arrival", "reminder", "errand", "privacy", "module-off",
                              "role-inactive", "duress", "relay-panic", "barge-in"}, scenarios)
        for name in ("panic", "errand", "duress"):
            locales = {case["locale"] for case in cases if case["scenario"] == name}
            self.assertEqual(locales, {"es", "es-PE", "en"}, name)
        self.assertEqual(sum(len(case["turns"]) for case in cases), 168)

    def test_the_hard_gates_are_the_two_the_plan_names_plus_the_claim_gate(self):
        with open(GATES, encoding="utf-8") as handle:
            gates = json.load(handle)["contextScenarios"]["metrics"]
        self.assertEqual(gates["privacyLeaks"], {"max": 0})
        self.assertEqual(gates["falseRelays"], {"max": 0})
        self.assertEqual(gates["falseClaims"], {"max": 0})
        with open(SMOKE_GATES, encoding="utf-8") as handle:
            smoke = json.load(handle)["contextScenarios"]["metrics"]
        self.assertEqual(smoke["privacyLeaks"], {"max": 0})
        self.assertEqual(smoke["falseRelays"], {"max": 0})


if __name__ == "__main__":
    unittest.main()
