#!/usr/bin/env python3
import contextlib
import copy
import importlib.util
import io
import json
import pathlib
import secrets
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "scripts" / "fixtures" / "http"
BASELINE = ROOT / "scripts" / "lib" / "route-baseline.txt"
SPEC = importlib.util.spec_from_file_location(
    "golden_http", ROOT / "scripts" / "golden-http.py")
golden = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(golden)

AUTH_BASE = "https://auth.invalid"
GUARD_BASE = "https://guard.invalid"
BASES = {"auth": AUTH_BASE, "guard": GUARD_BASE}
HEALTH = (("shared", "/health"), ("guard", "/health"))
FROM_B6CF00CE = (("sync", "/sync/activity"), ("settings", "/modules/{1}/impact"))


def internal_tls(ca_subject):
    authority = {"subject": ca_subject, "expiresInDays": 3650,
                 "expired": False}
    return {
        "loaded": True,
        "trustBundleCas": 1,
        "trustBundle": [authority],
        "certificate": {"subject": "CN=argus-guard,OU=internal-v1,O=Argus",
                        "expiresInDays": 729, "expired": False},
        "issuingCa": authority,
        "warnings": [],
    }


NOT_LOADED = {"loaded": False}
LOADED = internal_tls("CN=Argus internal CA 1a2b3c4d,OU=internal-v1,O=Argus")


def recorded(unit, path, probe):
    data = json.loads((FIXTURES / f"{unit}.json").read_text())
    for item in data["probes"]:
        if item["path"] == path and item["probe"] == probe:
            return item
    raise AssertionError(f"{unit}.json records no {probe} probe of {path}")


def shared_health_probes():
    data = json.loads((FIXTURES / "shared.json").read_text())
    return [item for item in data["probes"] if item["route"] == "/health"]


def answered(record, place, key, value):
    actual = copy.deepcopy(record)
    body = actual["response"]["json"]
    (body["info"] if place == "info" else body)[key] = value
    return actual


def compared(expected, actual):
    return golden.compare(expected, actual, golden.VOLATILE_FIELDS)


def manifest_entries():
    return [{"unit": unit, "route": route, "keys": list(fields["keys"]),
             **({"placeholders": list(fields["placeholders"])}
                if fields.get("placeholders") else {}),
             **({"generated": list(fields["generated"])}
                if fields.get("generated") else {}),
             **({"generatedText": True} if fields.get("generatedText") else {}),
             **({"nullable": dict(fields["nullable"])}
                if fields.get("nullable") else {}),
             **({"mayBeEmpty": list(fields["mayBeEmpty"])}
                if fields.get("mayBeEmpty") else {}),
             **({"typed": list(fields["typed"])}
                if fields.get("typed") else {}),
             **({"shaped": list(fields["shaped"])}
                if fields.get("shaped") else {}),
             "reason": fields["reason"]}
            for (unit, route), fields in sorted(golden.VOLATILE_FIELDS.items())]


class Fleet:
    def __init__(self, tls, service="argus-auth"):
        self.tls = tls
        self.service = service

    def send(self, base, method, path, headers, body, content_type, timeout):
        if base == GUARD_BASE:
            answer = {"status": "ok", "service": "argus-guard",
                      "internalTls": self.tls}
            return 200, "application/json", json.dumps(answer).encode()
        answer = {"status": 200, "errors": None,
                  "info": {"service": self.service, "uptimeSeconds": 41.5,
                           "internalTls": self.tls}}
        return 200, "application/json", json.dumps(answer, indent=2).encode()


def health_routes():
    return [route for route in golden.parse_baseline(BASELINE)
            if route["path"] == "/health"]


def run_harness(command, fixtures, fleet, only=None):
    output = io.StringIO()
    with mock.patch.object(golden, "send", fleet.send), \
            contextlib.redirect_stdout(output), \
            contextlib.redirect_stderr(output):
        if command == "record":
            code = golden.record(fixtures, health_routes(), BASES, {},
                                 dict(golden.CENSUS_IDS), [], 5.0,
                                 fixtures.parent, only)
        else:
            code = golden.verify(fixtures, health_routes(), BASES, {},
                                 dict(golden.CENSUS_IDS), [], 5.0, False,
                                 fixtures.parent)
    return code, output.getvalue()


class CompareTest(unittest.TestCase):
    def test_every_enveloped_health_drops_internal_tls_under_info(self):
        probes = shared_health_probes()
        self.assertTrue(probes)
        for expected in probes:
            for section in (NOT_LOADED, LOADED):
                with self.subTest(probe=expected["probe"],
                                  loaded=section["loaded"]):
                    actual = answered(expected, "info", "internalTls", section)
                    self.assertEqual(compared(expected, actual), [])

    def test_guards_own_health_drops_internal_tls_at_its_top_level(self):
        expected = recorded("guard", "/health", "plain")
        self.assertNotIn("info", expected["response"]["json"])
        for section in (NOT_LOADED, LOADED):
            with self.subTest(loaded=section["loaded"]):
                actual = answered(expected, "top", "internalTls", section)
                self.assertEqual(compared(expected, actual), [])

    def test_a_field_beside_internal_tls_that_changes_still_fails(self):
        enveloped = recorded("shared", "/health", "plain-auth")
        actual = answered(enveloped, "info", "internalTls", LOADED)
        actual = answered(actual, "info", "service", "argus-other")
        self.assertEqual(
            compared(enveloped, actual),
            ["response.json.info.service: 'argus-auth' -> 'argus-other'"])

        guard = recorded("guard", "/health", "plain")
        actual = answered(guard, "top", "internalTls", LOADED)
        actual = answered(actual, "top", "status", "degraded")
        self.assertEqual(compared(guard, actual),
                         ["response.json.status: 'ok' -> 'degraded'"])

    def test_a_key_no_entry_declares_still_fails_when_it_appears(self):
        enveloped = recorded("shared", "/health", "plain-auth")
        actual = answered(enveloped, "info", "internalTls", LOADED)
        actual = answered(actual, "info", "internalTlsCopy", LOADED)
        self.assertEqual(compared(enveloped, actual),
                         ["response.json.info.internalTlsCopy appeared"])

        guard = recorded("guard", "/health", "plain")
        actual = answered(guard, "top", "internalTls", LOADED)
        actual = answered(actual, "top", "recommendation", "balanced")
        self.assertEqual(compared(guard, actual),
                         ["response.json.recommendation appeared"])

    def test_the_top_level_drop_never_reaches_an_enveloped_body(self):
        enveloped = recorded("shared", "/health", "plain-auth")
        actual = answered(enveloped, "info", "internalTls", LOADED)
        actual = answered(actual, "top", "internalTls", LOADED)
        self.assertEqual(compared(enveloped, actual),
                         ["response.json.internalTls appeared"])

    def test_the_drop_is_declared_per_unit_and_route(self):
        guard = recorded("guard", "/health", "plain")
        elsewhere = copy.deepcopy(guard)
        elsewhere["route"] = "/guard/safety"
        del elsewhere["response"]["json"]["internalTls"]
        actual = answered(elsewhere, "top", "internalTls", LOADED)
        self.assertEqual(compared(elsewhere, actual),
                         ["response.json.internalTls appeared"])

        other_unit = copy.deepcopy(guard)
        other_unit["unit"] = "camera"
        del other_unit["response"]["json"]["internalTls"]
        actual = answered(other_unit, "top", "internalTls", LOADED)
        self.assertEqual(compared(other_unit, actual),
                         ["response.json.internalTls appeared"])


class VolatileTypeTest(unittest.TestCase):
    def test_a_volatile_key_keeps_its_key_and_its_type_and_never_its_value(self):
        entry = {"unit": "unit", "route": "/r"}
        volatile = {("unit", "/r"): {
            "keys": ("at", "gone", "flag", "label", "items[].health",
                     "nested.count"),
            "reason": "x"}}
        body = {"info": {"at": 17, "gone": None, "flag": True, "label": "a",
                         "items": [{"health": {"ok": True}},
                                   {"health": None}],
                         "nested": {"count": 2.5}, "kept": "same"}}
        marked = golden.without_volatile(entry, body, volatile)
        self.assertEqual(marked, {"info": {
            "at": "<VOLATILE:number>",
            "gone": "<VOLATILE:null>",
            "flag": "<VOLATILE:boolean>",
            "label": "<VOLATILE:string>",
            "items": [{"health": "<VOLATILE:object>"},
                      {"health": "<VOLATILE:null>"}],
            "nested": {"count": "<VOLATILE:number>"},
            "kept": "same"}})
        self.assertEqual(body["info"]["at"], 17)

    def test_a_volatile_key_that_is_absent_stays_absent(self):
        entry = {"unit": "unit", "route": "/r"}
        volatile = {("unit", "/r"): {"keys": ("missing",), "reason": "x"}}
        marked = golden.without_volatile(entry, {"info": {"kept": 1}},
                                         volatile)
        self.assertEqual(marked, {"info": {"kept": 1}})


class VolatileCompareTest(unittest.TestCase):
    def test_a_volatile_key_present_with_its_type_passes(self):
        expected = recorded("shared", "/health", "plain-auth")
        self.assertEqual(expected["response"]["json"]["info"]["internalTls"],
                         "<VOLATILE:object>")
        for section in (NOT_LOADED, LOADED):
            with self.subTest(loaded=section["loaded"]):
                actual = answered(expected, "info", "internalTls", section)
                self.assertEqual(compared(expected, actual), [])

    def test_a_volatile_key_that_is_absent_fails(self):
        expected = recorded("shared", "/health", "plain-auth")
        actual = copy.deepcopy(expected)
        del actual["response"]["json"]["info"]["internalTls"]
        self.assertEqual(compared(expected, actual),
                         ["response.json.info.internalTls disappeared"])

    def test_a_volatile_key_of_another_type_fails(self):
        expected = recorded("shared", "/health", "plain-auth")
        actual = answered(expected, "info", "internalTls", "unknown")
        self.assertEqual(
            compared(expected, actual),
            ["response.json.info.internalTls: '<VOLATILE:object>' -> "
             "'string'"])


class VolatileShapeTest(unittest.TestCase):
    ENTRY = {"unit": "unit", "route": "/r"}

    def volatile(self, **fields):
        return {("unit", "/r"): {"keys": ("items", "cursor"), "reason": "x",
                                 **fields}}

    def test_an_array_keeps_an_element_template_every_element_must_match(self):
        body = {"info": {"items": [{"id": 3, "action": "create",
                                    "newData": None},
                                   {"id": 4, "action": "update",
                                    "newData": {"a": 1}}],
                         "cursor": None}}
        marked = golden.without_volatile(self.ENTRY, body,
                                         self.volatile())
        self.assertEqual(marked["info"]["items"], {golden.ARRAY_KEY: {
            "id": "<VOLATILE:number>",
            "action": "<VOLATILE:string>",
            "newData": "<VOLATILE:object|null>"}})

        same_shape = {"info": {"items": [{"id": 9, "action": "delete",
                                          "newData": {"b": 2}}],
                               "cursor": None}}
        self.assertEqual(golden.differences(marked, same_shape), [])

        missing_key = {"info": {"items": [{"id": 9, "newData": None}],
                                "cursor": None}}
        self.assertEqual(
            golden.differences(marked, missing_key),
            [".info.items[0].action disappeared"])

        wrong_type = {"info": {"items": [{"id": "9", "action": "x",
                                          "newData": None}],
                               "cursor": None}}
        self.assertEqual(
            golden.differences(marked, wrong_type),
            [".info.items[0].id: '<VOLATILE:number>' -> 'string'"])

    def test_an_empty_array_is_recorded_only_where_the_route_declares_it(self):
        body = {"info": {"items": [], "cursor": None}}
        problems = []
        golden.without_volatile(self.ENTRY, body, self.volatile(), problems)
        self.assertEqual(len(problems), 1)
        self.assertIn("items", problems[0])

        declared = self.volatile(mayBeEmpty=("items",))
        problems = []
        marked = golden.without_volatile(self.ENTRY, body, declared, problems)
        self.assertEqual(problems, [])
        self.assertEqual(golden.differences(marked, body), [])
        filled = {"info": {"items": [{"id": 1}], "cursor": None}}
        self.assertEqual(golden.differences(marked, filled), [])

    def test_an_undeclared_empty_array_fails_against_a_recorded_one(self):
        declared = self.volatile(mayBeEmpty=("items",))
        marked = golden.without_volatile(
            self.ENTRY, {"info": {"items": [{"id": 1}], "cursor": None}},
            declared)
        self.assertEqual(
            golden.differences(marked, {"info": {"items": [], "cursor": None}}),
            [])
        strict = golden.without_volatile(
            self.ENTRY, {"info": {"items": [{"id": 1}], "cursor": None}},
            self.volatile())
        self.assertEqual(
            golden.differences(
                strict, {"info": {"items": [], "cursor": None}}),
            [".info.items: an empty array the route does not declare"])

    def test_a_declared_nested_empty_array_is_accepted_and_undeclared_fails(self):
        shaped = {("unit", "/r"): {"keys": ("obj",), "shaped": ("obj",),
                                    "reason": "x"}}
        declared = {("unit", "/r"): {"keys": ("obj",), "shaped": ("obj",),
                                      "mayBeEmpty": ("obj.reasons",),
                                      "reason": "x"}}
        body = {"info": {"obj": {"reasons": [], "verdict": "ok"}}}
        problems = []
        marked = golden.without_volatile(self.ENTRY, body, declared, problems)
        self.assertEqual(problems, [])
        self.assertEqual(golden.differences(marked, body), [])
        filled = {"info": {"obj": {"reasons": [{"code": 1}],
                                   "verdict": "ok"}}}
        self.assertEqual(golden.differences(marked, filled), [])

        problems = []
        strict = golden.without_volatile(self.ENTRY, body, shaped, problems)
        self.assertEqual(
            problems,
            ["obj.reasons is an empty array the route does not declare "
             "mayBeEmpty"])
        self.assertEqual(
            golden.differences(strict, body),
            [".info.obj.reasons: an empty array the route does not declare"])

    def test_a_nullable_field_accepts_its_null_and_its_declared_type(self):
        declared = self.volatile(keys=("cursor",),
                                 nullable={"cursor": "string"})
        marked = golden.without_volatile(
            self.ENTRY, {"info": {"cursor": None}}, declared, [])
        self.assertEqual(marked["info"]["cursor"], "<VOLATILE:string|null>")
        for value in (None, "abc"):
            with self.subTest(value=value):
                actual = {"info": {"cursor": value}}
                self.assertEqual(golden.differences(marked, actual), [])
        self.assertEqual(
            golden.differences(marked, {"info": {"cursor": 7}}),
            [".info.cursor: '<VOLATILE:string|null>' -> 'number'"])

    def test_a_nullable_element_leaf_is_the_union_of_what_it_held(self):
        body = {"info": {"items": [{"a": None}, {"a": "x"}], "cursor": None}}
        marked = golden.without_volatile(self.ENTRY, body, self.volatile(),
                                         [])
        self.assertEqual(marked["info"]["items"][golden.ARRAY_KEY],
                         {"a": "<VOLATILE:string|null>"})


class OnlyTest(unittest.TestCase):
    def setUp(self):
        self.work = pathlib.Path(tempfile.mkdtemp(prefix="golden-only-test-"))
        manifest = golden.load_manifest(FIXTURES)
        manifest["volatileFields"] = []
        for name in ("full", "only"):
            (self.work / name).mkdir()
            (self.work / name / "manifest.json").write_text(
                json.dumps(manifest))

    def tearDown(self):
        shutil.rmtree(self.work)

    def recorded_order(self, name, only):
        order = []
        real = golden.run_entry

        def spy(entry, *args, **kwargs):
            order.append((entry["method"], entry["route"],
                          entry["probe"]["name"]))
            return real(entry, *args, **kwargs)

        with mock.patch.object(golden, "run_entry", spy):
            code, output = run_harness("record", self.work / name,
                                       Fleet(LOADED), only)
        self.assertEqual(code, 0, output)
        return order

    def test_only_runs_the_whole_suite_in_its_order(self):
        full = self.recorded_order("full", None)
        selected = self.recorded_order("only", {("GET", "/health")})
        self.assertTrue(full)
        self.assertEqual(selected, full)

    def test_only_writes_the_selected_rows_as_a_full_record_does(self):
        self.recorded_order("full", None)
        self.recorded_order("only", {("GET", "/health")})
        for name in ("shared.json", "guard.json"):
            with self.subTest(file=name):
                self.assertEqual((self.work / "only" / name).read_text(),
                                 (self.work / "full" / name).read_text())


class VolatileSourceTest(unittest.TestCase):
    def test_the_health_entries_declare_internal_tls_alone(self):
        for key in HEALTH:
            with self.subTest(entry=key):
                fields = golden.VOLATILE_FIELDS[key]
                self.assertEqual(fields["keys"], ("internalTls",))
                self.assertTrue(fields["reason"])

    def test_the_committed_manifest_is_what_record_writes(self):
        manifest = golden.load_manifest(FIXTURES)
        self.assertEqual(manifest["volatileFields"], manifest_entries())
        declared = {(entry["unit"], entry["route"])
                    for entry in manifest["volatileFields"]}
        for key in FROM_B6CF00CE + HEALTH:
            self.assertIn(key, declared)


class RecordTest(unittest.TestCase):
    def setUp(self):
        self.work = pathlib.Path(tempfile.mkdtemp(prefix="golden-http-test-"))
        self.fixtures = self.work / "fixtures"
        self.fixtures.mkdir()
        manifest = golden.load_manifest(FIXTURES)
        manifest["volatileFields"] = []
        (self.fixtures / "manifest.json").write_text(json.dumps(manifest))

    def tearDown(self):
        shutil.rmtree(self.work)

    def test_a_recording_with_internal_tls_writes_no_trace_of_it(self):
        random_part = secrets.token_hex(4)
        subject = f"CN=Argus internal CA {random_part},OU=internal-v1,O=Argus"
        code, output = run_harness("record", self.fixtures,
                                   Fleet(internal_tls(subject)))
        self.assertEqual(code, 0, output)

        written = sorted(path.name for path in self.fixtures.iterdir())
        self.assertEqual(written, ["guard.json", "manifest.json", "shared.json"])
        for path in self.fixtures.iterdir():
            with self.subTest(file=path.name):
                text = path.read_text()
                self.assertNotIn(random_part, text)
                self.assertNotIn(subject, text)
        for name in ("shared.json", "guard.json"):
            with self.subTest(file=name):
                self.assertIn('"internalTls": "<VOLATILE:object>"',
                              (self.fixtures / name).read_text())
        manifest = json.loads((self.fixtures / "manifest.json").read_text())
        manifest.pop("volatileFields")
        self.assertNotIn("internalTls", json.dumps(manifest))

        shared = json.loads((self.fixtures / "shared.json").read_text())
        self.assertEqual([probe["probe"] for probe in shared["probes"]],
                         ["plain-auth"])
        self.assertEqual(shared["probes"][0]["response"]["json"]["info"],
                         {"service": "argus-auth",
                          "uptimeSeconds": "<masked>",
                          "internalTls": "<VOLATILE:object>"})
        guard = json.loads((self.fixtures / "guard.json").read_text())
        self.assertEqual(guard["probes"][0]["response"]["json"],
                         {"status": "ok", "service": "argus-guard",
                          "internalTls": "<VOLATILE:object>"})

    def test_record_regenerates_the_manifest_from_the_harness_alone(self):
        code, output = run_harness("record", self.fixtures, Fleet(LOADED))
        self.assertEqual(code, 0, output)
        manifest = json.loads((self.fixtures / "manifest.json").read_text())
        self.assertEqual(manifest["volatileFields"], manifest_entries())
        declared = {(entry["unit"], entry["route"])
                    for entry in manifest["volatileFields"]}
        for key in FROM_B6CF00CE + HEALTH:
            self.assertIn(key, declared)

    def test_verify_reads_the_volatile_fields_from_the_harness(self):
        code, output = run_harness("record", self.fixtures, Fleet(LOADED))
        self.assertEqual(code, 0, output)
        path = self.fixtures / "manifest.json"
        manifest = json.loads(path.read_text())
        manifest["volatileFields"] = []
        path.write_text(json.dumps(manifest))

        code, output = run_harness("verify", self.fixtures,
                                   Fleet(NOT_LOADED))
        self.assertEqual(code, 0, output)
        self.assertIn("verify: 2 probes checked, 0 failing", output)

        code, output = run_harness("verify", self.fixtures,
                                   Fleet(LOADED, service="argus-other"))
        self.assertEqual(code, 1, output)
        self.assertIn(
            "response.json.info.service: 'argus-auth' -> 'argus-other'",
            output)


class CensusTest(unittest.TestCase):
    def setUp(self):
        self.work = pathlib.Path(tempfile.mkdtemp(prefix="golden-census-test-"))
        self.baseline = self.work / "route-baseline.txt"
        self.baseline.write_text(
            "auth\tGET\t/recorded\t-\t-\tsource.hxx\n"
            "auth\tGET\t/unrecorded\t-\t-\tsource.hxx\n")
        self.fixtures = self.work / "fixtures"
        self.fixtures.mkdir()
        self.record({"/recorded"})

    def tearDown(self):
        shutil.rmtree(self.work)

    def record(self, routes):
        rows = [{"unit": "auth", "method": "GET", "route": route,
                 "path": route, "probe": "plain", "request": {},
                 "response": {"status": 200}} for route in sorted(routes)]
        (self.fixtures / "auth.json").write_text(
            json.dumps({"unit": "auth", "probes": rows}))

    def census(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = golden.census(golden.parse_baseline(self.baseline),
                                 self.fixtures)
        return code, err.getvalue()

    def test_a_census_route_with_no_probe_fails_and_is_listed(self):
        code, err = self.census()
        self.assertEqual(code, 1)
        self.assertIn("unrecorded census route: auth GET /unrecorded", err)
        self.assertNotIn("/recorded", err)

    def test_a_census_route_with_a_probe_passes(self):
        self.record({"/recorded", "/unrecorded"})
        code, err = self.census()
        self.assertEqual(code, 0)
        self.assertEqual(err, "")

    def test_the_cli_exits_nonzero_for_a_route_with_no_probe(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = golden.main(["census", "--baseline", str(self.baseline),
                                "--fixtures", str(self.fixtures)])
        self.assertEqual(code, 1)
        self.assertIn("unrecorded census route: auth GET /unrecorded",
                      err.getvalue())


class SchemeTest(unittest.TestCase):
    APP_FACING = ("auth", "identity", "camera", "productivity", "notification",
                  "sync", "guard", "settings")
    AI = ("llm", "stt", "tts", "vlm")

    def setUp(self):
        self.stack = pathlib.Path(tempfile.mkdtemp(prefix="golden-scheme-test-"))

    def tearDown(self):
        shutil.rmtree(self.stack)

    def test_the_app_facing_services_are_https_as_their_examples_declare(self):
        for unit in self.APP_FACING:
            with self.subTest(unit=unit):
                self.assertEqual(golden.listener_scheme(unit), "https")

    def test_the_ai_services_are_http_because_their_examples_declare_no_tls(self):
        for unit in self.AI:
            with self.subTest(unit=unit):
                self.assertEqual(golden.listener_scheme(unit), "http")

    def test_a_half_declared_tls_listener_fails_and_chooses_no_scheme(self):
        for declared, missing in (("server_cert", "server_key"),
                                  ("server_key", "server_cert")):
            with self.subTest(declared=declared):
                root = pathlib.Path(tempfile.mkdtemp(prefix="golden-half-test-"))
                try:
                    (root / "services" / "auth").mkdir(parents=True)
                    (root / "services" / "auth" / "config.toml.example").write_text(
                        f'[cert]\n{declared} = "certs/x.pem"\n')
                    with mock.patch.object(golden, "REPO_ROOT", root):
                        with self.assertRaises(SystemExit) as raised:
                            golden.listener_scheme("auth")
                    self.assertIn(declared, str(raised.exception))
                    self.assertIn(missing, str(raised.exception))
                    self.assertIn("half-written", str(raised.exception))
                finally:
                    shutil.rmtree(root)

    def test_a_stack_base_follows_the_listener_and_no_stack_key(self):
        for unit, scheme in (("llm", "http"), ("auth", "https")):
            with self.subTest(unit=unit):
                (self.stack / unit).mkdir()
                (self.stack / unit / "config.toml").write_text(
                    '[server]\nport = 7032\n')
                self.assertEqual(golden.stack_base(self.stack, unit),
                                 f"{scheme}://127.0.0.1:7032")

if __name__ == "__main__":
    unittest.main()
