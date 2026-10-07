import json
import os
import pathlib
import subprocess
import unittest

CLI = os.environ.get("CALL_TIME_CLI", "")
NOW = "2026-10-07T15:20:00+00:00"


def ask(*requests):
    process = subprocess.run([CLI], input="".join(json.dumps(r) + "\n" for r in requests), capture_output=True, text=True,
                             timeout=30, env={**os.environ, "TZ": "UTC"})
    return process.returncode, [json.loads(line) for line in process.stdout.splitlines()]


@unittest.skipUnless(CLI and pathlib.Path(CLI).exists(), "the call-time-cli binary is not built")
class CallTimeCliTest(unittest.TestCase):
    def test_a_clock_time_resolves_to_an_instant_and_names_its_phrase(self):
        code, answers = ask({"seq": 1, "text": "llámame a las 18:30", "lang": "es", "now": NOW})
        self.assertEqual(code, 0)
        self.assertEqual(answers[0]["seq"], 1)
        self.assertEqual(answers[0]["at"], "2026-10-07T18:30:00+00:00")
        self.assertLess(answers[0]["begin"], answers[0]["end"])

    def test_text_without_a_time_answers_null_and_keeps_the_sequence(self):
        _, answers = ask({"seq": 7, "text": "la agenda del congreso", "lang": "es", "now": NOW})
        self.assertEqual(answers, [{"seq": 7, "at": None}])

    def test_a_clock_that_cannot_be_read_answers_null(self):
        _, answers = ask({"seq": 2, "text": "a las nueve", "lang": "es", "now": "not a time"})
        self.assertEqual(answers[0]["at"], None)

    def test_each_request_gets_its_own_answer_in_order(self):
        _, answers = ask({"seq": 1, "text": "a las nueve", "lang": "es", "now": NOW},
                         {"seq": 2, "text": "at five tomorrow", "lang": "en", "now": NOW})
        self.assertEqual([a["seq"] for a in answers], [1, 2])

    def test_a_line_that_is_not_json_stops_the_process_with_an_error(self):
        process = subprocess.run([CLI], input="{\n", capture_output=True, text=True, timeout=30)
        self.assertEqual(process.returncode, 1)


if __name__ == "__main__":
    unittest.main()
