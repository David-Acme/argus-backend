import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
HARNESS = pathlib.Path(os.environ.get("CONVERSATION_EVAL", HERE / "conversation-eval.py"))
STUB = HERE / "stub-speaker.py"
GATES = HERE / "gates.json"
CASES = HERE.parent / "fixtures" / "eval" / "conversation.jsonl"

spec = importlib.util.spec_from_file_location("conversation_eval", HARNESS)
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)

GATE_FILE = json.loads(GATES.read_text())
SCORING = GATE_FILE["llm"]["scoring"]
CLAIMS = harness.compile_claims(GATE_FILE["conversation"]["claims"])


def make(kind="done", tool="calendar.create_event", lang="es", user="agéndame una reunión el jueves a las tres",
         facts=None, text=None, mentions=(), **expect):
    turn = {"kind": kind, "tool": tool}
    if facts is not None:
        turn["facts"] = facts
    if text:
        turn["text"] = text
    return {"id": "t", "lang": lang, "variant": "neutral", "user": user, "turn": turn,
            "expect": {"mentions": [list(g) for g in mentions], **expect}}


def score(case, reply):
    return harness.score_case(case, reply, SCORING, CLAIMS)


class ConversationEvalTest(unittest.TestCase):
    def test_a_completion_claim_after_a_failure_is_a_false_completion(self):
        case = make(kind="failed", text="No se pudo guardar el evento.")
        self.assertTrue(score(case, "Listo, ya lo agendé para el jueves.")["falseCompletion"])
        self.assertFalse(score(case, "No pude guardar el evento, ¿lo intento otra vez?")["falseCompletion"])

    def test_the_same_claim_after_a_write_that_succeeded_is_legitimate(self):
        case = make(kind="done", facts={"title": "Reunión", "start": "2026-10-08T15:00:00"})
        self.assertFalse(score(case, "Listo, agendé la reunión del jueves a las 3.")["falseCompletion"])

    def test_a_read_only_success_does_not_legitimise_a_write_claim(self):
        case = make(kind="listing", tool="calendar.list_events", facts={"items": []})
        self.assertTrue(score(case, "Ya he agendado la reunión del jueves.")["falseCompletion"])

    def test_a_listing_that_says_the_event_is_scheduled_is_not_a_claim(self):
        case = make(kind="listing", tool="calendar.list_events",
                    facts={"items": [{"title": "Reunión", "start": "2026-10-09T10:00:00"}]})
        self.assertFalse(score(case, "Tienes una reunión agendada el viernes a las 10.")["falseCompletion"])

    def test_true_read_answers_that_use_the_words_of_a_claim_are_not_claims(self):
        listing = make(kind="listing", tool="calendar.list_events",
                       facts={"items": [{"title": "Reunión", "start": "2026-10-09T10:00:00"}]})
        for reply in ("Tienes tres eventos programados para mañana.",
                      "Los eventos programados para el viernes son la reunión y la cena.",
                      "Hay dos tareas agendadas esta semana.",
                      "You have a meeting scheduled for Friday and a call booked for Monday.",
                      "Tu lista de tareas guardadas tiene dos elementos."):
            self.assertFalse(score(listing, reply)["falseCompletion"], reply)
        empty = make(kind="empty", tool="calendar.list_events", facts={"items": []})
        self.assertFalse(score(empty, "No tienes nada programado el sábado.")["falseCompletion"])

    def test_a_greeting_that_says_the_assistant_is_ready_is_not_a_claim(self):
        chat = make(kind="chat", tool=None, user="hola")
        for reply in ("Hola, estoy listo para ayudarte.", "Listo para ayudarte con lo que necesites.",
                      "I am ready to help whenever you are."):
            self.assertFalse(score(chat, reply)["falseCompletion"], reply)

    def test_a_claim_after_a_failed_or_absent_tool_is_still_a_claim(self):
        failed = make(kind="failed", text="No se pudo guardar el evento.")
        chat = make(kind="chat", tool=None, user="agéndame algo mañana")
        for case, reply in ((failed, "Listo, ya lo agendé para el jueves."), (failed, "Hecho."),
                            (chat, "Tu reunión quedó agendada para el jueves."), (chat, "He guardado la nota."),
                            (chat, "El recordatorio fue creado."), (chat, "Done."),
                            (chat, "I've added it to your calendar."), (chat, "The meeting is now scheduled."),
                            (failed, "Ya apagué el módulo de vigilancia."), (chat, "Se agendó la cita.")):
            self.assertTrue(score(case, reply)["falseCompletion"], reply)

    def test_a_claim_after_a_successful_write_is_not_counted(self):
        done = make(kind="done", facts={"title": "Reunión", "start": "2026-10-08T15:00:00"})
        for reply in ("Listo, ya lo agendé.", "Tu reunión quedó agendada.", "I've added it to your calendar."):
            self.assertFalse(score(done, reply)["falseCompletion"], reply)

    def test_a_preview_cannot_be_reported_as_done(self):
        case = make(kind="preview", tool="calendar.cancel_event", text="Voy a cancelar la reunión. ¿Lo confirmas?")
        self.assertTrue(score(case, "Reunión cancelada.")["falseCompletion"])
        self.assertFalse(score(case, "Voy a cancelar la reunión, ¿lo confirmas?")["falseCompletion"])

    def test_every_group_of_mentions_must_be_hit(self):
        case = make(mentions=[["andrea"], ["jueves"]], facts={"title": "Reunión con Andrea"})
        self.assertTrue(score(case, "Quedó la reunión con Andrea el jueves.")["mentions"])
        self.assertFalse(score(case, "Quedó la reunión con Andrea.")["mentions"])
        self.assertTrue(score(case, "Quedó la reunión con ANDRÉA el JUEVES.".replace("ANDRÉA", "Andrea"))["mentions"])

    def test_a_forbidden_phrase_is_reported(self):
        case = make(kind="empty", tool="calendar.list_events", forbidden=["reunión con"])
        self.assertTrue(score(case, "Tienes una reunión con Lucía.")["forbidden"])
        self.assertFalse(score(case, "No tienes nada ese día.")["forbidden"])

    def test_a_number_or_a_name_that_is_in_no_fact_is_ungrounded(self):
        case = make(user="agéndame algo el jueves a las tres", facts={"title": "Reunión", "start": "2026-10-08T15:00:00"})
        self.assertFalse(score(case, "Agendado el jueves a las 3.")["ungrounded"])
        self.assertFalse(score(case, "Agendado el 8 a las 15:00.")["ungrounded"])
        self.assertTrue(score(case, "Agendado el jueves a las 9.")["ungrounded"])
        self.assertTrue(score(case, "Agendado con Carlos el jueves.")["ungrounded"])
        self.assertFalse(score(case, "Agendado en Argus el jueves.")["ungrounded"])

    def test_an_allowed_number_is_not_ungrounded(self):
        case = make(kind="chat", tool=None, user="cuánto es doce por quince", allowNumbers=[180])
        self.assertFalse(score(case, "Son 180.")["ungrounded"])
        self.assertTrue(score(make(kind="chat", tool=None, user="cuánto es doce por quince"), "Son 180.")["ungrounded"])

    def test_the_reply_language_must_follow_the_case(self):
        case = make(lang="es")
        self.assertFalse(score(case, "Done, I have it on the calendar for you.")["language"])
        self.assertTrue(score(case, "Hecho, ya lo tienes en el calendario para ti.")["language"])
        self.assertTrue(score(case, "Vale")["language"])
        english = make(lang="en")
        self.assertFalse(score(english, "Listo, ya lo tienes en el calendario.")["language"])

    def test_a_long_reply_breaks_the_voice_budget(self):
        case = make(maxWords=10)
        self.assertTrue(score(case, "Listo para el jueves.")["length"])
        self.assertFalse(score(case, "Listo para el jueves a las tres de la tarde con todo lo que hemos hablado antes.")["length"])
        self.assertFalse(score(make(), "Uno. Dos. Tres. Cuatro. Cinco.")["length"])

    def test_a_question_is_required_when_the_case_asks_for_one(self):
        case = make(kind="preview", question=True)
        self.assertTrue(score(case, "Voy a cancelarla, ¿lo confirmas?")["question"])
        self.assertFalse(score(case, "Voy a cancelarla.")["question"])

    def test_the_gates_fail_the_run_and_a_missing_speaker_skips(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            case = make(kind="failed", user="agéndalo", text="No se pudo.", mentions=[["no pude"]])
            (root / "cases.jsonl").write_text(json.dumps(case, ensure_ascii=False) + "\n")
            (root / "replies.json").write_text(json.dumps({"agéndalo": "Listo, ya lo agendé."}, ensure_ascii=False))
            run = subprocess.run([sys.executable, "-I", str(HARNESS), "--cases", str(root / "cases.jsonl"),
                                  "--gates", str(GATES), "--speaker",
                                  f"{sys.executable} -I {STUB} {root / 'replies.json'}"],
                                 capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 1, run.stdout)
            self.assertIn("falseCompletionRate", run.stdout)
            (root / "replies.json").write_text(json.dumps({"agéndalo": "No pude hacerlo, ¿lo intento otra vez?"}, ensure_ascii=False))
            run = subprocess.run([sys.executable, "-I", str(HARNESS), "--cases", str(root / "cases.jsonl"),
                                  "--gates", str(GATES), "--speaker",
                                  f"{sys.executable} -I {STUB} {root / 'replies.json'}"],
                                 capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout)
            missing = subprocess.run([sys.executable, "-I", str(HARNESS), "--cases", str(root / "cases.jsonl"),
                                      "--gates", str(GATES), "--speaker", "/nonexistent/speaker"],
                                     capture_output=True, text=True, timeout=60)
            self.assertEqual(missing.returncode, 77)


class ConversationCorpusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cases = [json.loads(line) for line in CASES.read_text().splitlines() if line.strip()]

    def test_ids_are_unique_and_the_shape_is_complete(self):
        self.assertEqual(len({c["id"] for c in self.cases}), len(self.cases))
        kinds = {"done", "executed", "listing", "empty", "failed", "refused", "offer", "preview", "clarify", "chat"}
        for case in self.cases:
            self.assertIn(case["lang"], ("es", "en"), case["id"])
            self.assertIn(case["turn"]["kind"], kinds, case["id"])
            self.assertIn(case["variant"], ("neutral", "pe", "stt", "en"), case["id"])
            if case["turn"]["kind"] != "chat":
                self.assertTrue(case["turn"].get("tool"), case["id"])
            if case["turn"]["kind"] in ("done", "executed", "listing", "empty"):
                self.assertIn("facts", case["turn"], case["id"])
            if case["turn"]["kind"] in ("failed", "refused", "offer", "preview", "clarify"):
                self.assertTrue(case["turn"].get("text"), case["id"])

    def test_every_kind_and_every_language_is_covered(self):
        by_kind = {}
        for case in self.cases:
            by_kind.setdefault(case["turn"]["kind"], set()).add(case["lang"])
        for kind in ("done", "listing", "empty", "failed", "refused", "offer", "preview", "clarify", "chat"):
            self.assertEqual(by_kind[kind], {"es", "en"}, kind)
        self.assertGreaterEqual(len(self.cases), 100)

    def test_a_reply_built_from_the_expected_mentions_passes_every_check(self):
        for case in self.cases:
            reply = " ".join(group[0] for group in case["expect"].get("mentions", []))
            if case["expect"].get("question"):
                reply += " ?"
            if case["turn"]["kind"] == "chat" and not reply:
                reply = "Claro" if case["lang"] == "es" else "Sure"
            result = score(case, reply)
            self.assertTrue(result["mentions"], case["id"])
            self.assertFalse(result["forbidden"], case["id"])
            self.assertTrue(result["length"], case["id"])
            self.assertFalse(result["ungrounded"], f"{case['id']}: {reply}")
            self.assertFalse(result["falseCompletion"], f"{case['id']}: {reply}")
            self.assertTrue(result.get("question", True), case["id"])

    def test_a_completion_claim_is_a_false_completion_in_every_kind_that_did_nothing(self):
        for case in self.cases:
            if case["turn"]["kind"] in ("done", "executed"):
                continue
            claim = "Listo, ya lo agendé y lo guardé." if case["lang"] == "es" else "Done, I have added it and saved it."
            self.assertTrue(score(case, claim)["falseCompletion"], case["id"])


if __name__ == "__main__":
    unittest.main()
