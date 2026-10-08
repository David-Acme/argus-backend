import os
import pathlib
import subprocess
import tempfile
import unittest

EVAL = os.environ.get("CALL_FAITHFULNESS_EVAL", "")
GATES = os.environ.get("CALL_FAITHFULNESS_GATES", "")
SMOKE_GATES = os.environ.get("CALL_FAITHFULNESS_SMOKE_GATES", "")
MISSING_MODEL = os.environ.get("CALL_FAITHFULNESS_MISSING_MODEL", "")


def run(gates, arguments):
    return subprocess.run([EVAL, "--gates", gates, "--llm-model", MISSING_MODEL, *arguments],
                          capture_output=True, text=True, timeout=120)


@unittest.skipUnless(EVAL and pathlib.Path(EVAL).exists(), "the call-faithfulness-eval binary is not built")
class CallFaithfulnessPinTest(unittest.TestCase):
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
            pinless.write_text('{"callFaithfulness": {"metrics": {"cases": {"min": 1}}}}', encoding="utf-8")
            process = run(str(pinless), ["--temperature", "0.3", "--seed", "42", "--force"])
            self.assertEqual(process.returncode, 1)
            self.assertIn("without the temperature and seed", process.stdout)


if __name__ == "__main__":
    unittest.main()
