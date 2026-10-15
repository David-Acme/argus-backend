#!/usr/bin/env python3
import contextlib
import copy
import importlib.util
import io
import json
import pathlib
import re
import secrets
import shutil
import sys
import tempfile
import unittest
import urllib.error
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
             **({"enums": {path: {"file": decl["file"],
                                  "values": list(decl["values"])}
                           for path, decl in fields["enums"].items()}}
                if fields.get("enums") else {}),
             **({"nullTemplates": {path: {"file": decl["file"],
                                          "struct": decl["struct"],
                                          "fields": dict(decl["fields"])}
                                   for path, decl in
                                   fields["nullTemplates"].items()}}
                if fields.get("nullTemplates") else {}),
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

    def test_a_masked_number_keeps_its_number_type_in_a_template(self):
        self.assertEqual(golden.normalize({"createdAt": 17}),
                         {"createdAt": "<masked:number>"})
        body = {"info": {"items": [{"createdAt": 17, "id": 3},
                                   {"createdAt": 1700000000, "id": 4}],
                         "cursor": None}}
        marked = golden.without_volatile(
            self.ENTRY, golden.normalize(body), self.volatile())
        self.assertEqual(
            marked["info"]["items"][golden.ARRAY_KEY]["createdAt"],
            "<VOLATILE:number>")
        live = golden.normalize({"info": {"items": [{"createdAt": 9, "id": 1}],
                                          "cursor": None}})
        self.assertEqual(golden.differences(marked, live), [])
        wrong = golden.normalize({"info": {"items": [{"createdAt": "9",
                                                      "id": 1}],
                                           "cursor": None}})
        self.assertEqual(
            golden.differences(marked, wrong),
            [".info.items[0].createdAt: '<VOLATILE:number>' -> 'string'"])

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


class TimingOutFleet(Fleet):
    def send(self, base, method, path, headers, body, content_type, timeout):
        if base == GUARD_BASE:
            return golden.TIMED_OUT, "text/plain", b"timed out"
        return super().send(base, method, path, headers, body, content_type,
                            timeout)


class TimedOutProbeTest(unittest.TestCase):
    def setUp(self):
        self.work = pathlib.Path(tempfile.mkdtemp(prefix="golden-timeout-test-"))
        self.fixtures = self.work / "fixtures"
        self.fixtures.mkdir()
        manifest = golden.load_manifest(FIXTURES)
        manifest["volatileFields"] = []
        (self.fixtures / "manifest.json").write_text(json.dumps(manifest))

    def tearDown(self):
        shutil.rmtree(self.work)

    def test_a_read_that_times_out_is_a_zero_status_answer(self):
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.read.side_effect = TimeoutError("timed out")
        with mock.patch.object(golden.urllib.request, "urlopen",
                               return_value=response):
            status, _, raw = golden.send(GUARD_BASE, "GET", "/health", {},
                                         None, None, 1.0)
        self.assertEqual(status, golden.TIMED_OUT)
        self.assertIn(b"timed out after 1.0s", raw)

    def test_a_connect_that_times_out_is_a_zero_status_answer(self):
        with mock.patch.object(
                golden.urllib.request, "urlopen",
                side_effect=urllib.error.URLError(TimeoutError("timed out"))):
            status, _, _ = golden.send(GUARD_BASE, "GET", "/health", {}, None,
                                       None, 1.0)
        self.assertEqual(status, golden.TIMED_OUT)

    def test_an_unreachable_service_still_stops_the_run(self):
        with mock.patch.object(
                golden.urllib.request, "urlopen",
                side_effect=urllib.error.URLError(
                    ConnectionRefusedError("refused"))):
            with self.assertRaises(SystemExit):
                golden.send(GUARD_BASE, "GET", "/health", {}, None, None, 1.0)

    def test_verify_counts_a_timed_out_probe_as_failing_and_finishes(self):
        code, output = run_harness("record", self.fixtures, Fleet(LOADED))
        self.assertEqual(code, 0, output)
        code, output = run_harness("verify", self.fixtures,
                                   TimingOutFleet(LOADED))
        self.assertEqual(code, 1, output)
        self.assertIn("response.status: 200 -> 0", output)
        self.assertIn("verify: 2 probes checked, 1 failing", output)


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
                          "uptimeSeconds": "<masked:number>",
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

VOLATILE_PREFIX = "<VOLATILE:"
VOLATILE_TYPES = ("array", "boolean", "null", "number", "object", "string")
ARRAY_MARKER = "<VOLATILE:array>"
EMPTY_MARKER = "<VOLATILE:mayBeEmpty>"
OR_NULL_MARKER = "<VOLATILE:or-null>"
ENUM_PREFIX = "<VOLATILE:enum:"
MASKED_PREFIX = "<masked:"
PLAIN_MARKERS = ("<GENERATED>", "<BYTES>", "<masked-hex32>",
                 "<masked-hex64>", "<masked-token>", "<STACK>", "<ROOT>")
MARKER_SHAPE = re.compile(r"^<[A-Za-z][A-Za-z:_|\-]*>$")
CPP_TYPES = {"std::string": "string", "int": "number", "double": "number",
             "bool": "boolean"}


def volatile_scalar_problem(value):
    if value == ARRAY_MARKER:
        return f"{value!r} is a bare array marker; an array is an element template"
    if not value.endswith(">"):
        return f"unknown marker {value!r}"
    body = value[len(VOLATILE_PREFIX):-1]
    if value.startswith(ENUM_PREFIX):
        members = value[len(ENUM_PREFIX):-1].split("|")
        if not all(members) or len(set(members)) != len(members):
            return f"enum marker {value!r} needs a non-empty set of distinct members"
        return None
    names = body.split("|")
    if not body or any(name not in VOLATILE_TYPES for name in names):
        return f"unknown marker {value!r}"
    if len(set(names)) != len(names) or ("null" in names
                                         and names[-1] != "null"):
        return f"unknown marker {value!r}"
    return None


def grammar_problems(value, path="$"):
    if isinstance(value, str):
        if value.startswith(VOLATILE_PREFIX):
            problem = volatile_scalar_problem(value)
            return [f"{path}: {problem}"] if problem else []
        if value.startswith(MASKED_PREFIX):
            name = value[len(MASKED_PREFIX):-1] if value.endswith(">") else ""
            if name in VOLATILE_TYPES:
                return []
            return [f"{path}: unknown marker {value!r}"]
        if MARKER_SHAPE.match(value) and value not in PLAIN_MARKERS:
            return [f"{path}: unknown marker {value!r}"]
        return []
    if isinstance(value, dict):
        volatile = [key for key in value if key.startswith(VOLATILE_PREFIX)]
        if not volatile:
            found = []
            for key, child in value.items():
                found.extend(grammar_problems(child, f"{path}.{key}"))
            return found
        if OR_NULL_MARKER in value:
            if set(value) != {OR_NULL_MARKER}:
                return [f"{path}: {OR_NULL_MARKER} holds only its template"]
            template = value[OR_NULL_MARKER]
            if not isinstance(template, dict) or any(
                    key.startswith(VOLATILE_PREFIX) for key in template):
                return [f"{path}: {OR_NULL_MARKER} needs a plain object template"]
            return grammar_problems(template, f"{path}?")
        found = [f"{path}: unknown marker key {key!r}" for key in volatile
                 if key not in (ARRAY_MARKER, EMPTY_MARKER)]
        if set(value) - {ARRAY_MARKER, EMPTY_MARKER}:
            found.append(f"{path}: a marker object holds keys other than "
                         "its marker")
        if ARRAY_MARKER not in value:
            found.append(f"{path}: {EMPTY_MARKER} without {ARRAY_MARKER}")
        if EMPTY_MARKER in value and value[EMPTY_MARKER] is not True:
            found.append(f"{path}: {EMPTY_MARKER} must be true")
        template = value.get(ARRAY_MARKER)
        if template is not None:
            found.extend(grammar_problems(template, f"{path}[]"))
        return found
    if isinstance(value, list):
        found = []
        for index, child in enumerate(value):
            found.extend(grammar_problems(child, f"{path}[{index}]"))
        return found
    return []


def fixture_grammar_problems():
    problems = []
    for path in sorted(FIXTURES.glob("*.json")):
        if path.name == "manifest.json":
            continue
        for probe in json.loads(path.read_text())["probes"]:
            where = (f"{path.stem} {probe['method']} {probe['route']} "
                     f"[{probe['probe']}]")
            response = probe["response"]
            for problem in grammar_problems(response.get("json"),
                                            f"{where} response.json"):
                problems.append(problem)
            text = response.get("text")
            if isinstance(text, str) and MARKER_SHAPE.match(text) \
                    and text not in PLAIN_MARKERS:
                problems.append(f"{where} response.text: unknown marker "
                                f"{text!r}")
    return problems


def enum_declarations():
    return [(unit, route, path, decl)
            for (unit, route), fields in sorted(golden.VOLATILE_FIELDS.items())
            for path, decl in sorted(fields.get("enums", {}).items())]


def null_template_declarations():
    return [(unit, route, path, decl)
            for (unit, route), fields in sorted(golden.VOLATILE_FIELDS.items())
            for path, decl in sorted(fields.get("nullTemplates", {}).items())]


def struct_fields(source, name):
    body = re.search(rf"struct {name}\s*\{{(.*?)\n\}};", source, re.S)
    if body is None:
        return None
    fields = {}
    for kind, field in re.findall(r"(std::string|int|double|bool)\s+(\w+)\s*[;{{]",
                                  body.group(1)):
        fields[field] = CPP_TYPES[kind]
    return fields


class VolatileSourceDriftTest(unittest.TestCase):
    def test_every_enum_value_is_a_quoted_member_of_its_source_file(self):
        found = enum_declarations()
        self.assertTrue(found)
        for unit, route, path, decl in found:
            source = (ROOT / decl["file"]).read_text()
            for value in decl["values"]:
                with self.subTest(enum=path, value=value):
                    self.assertIn(f'"{value}"', source,
                                  f"{decl['file']} has no string {value!r}")

    def test_every_enum_and_template_sits_under_a_shaped_object(self):
        for (unit, route), fields in golden.VOLATILE_FIELDS.items():
            shaped = fields.get("shaped", ())
            for path in list(fields.get("enums", {})) + list(
                    fields.get("nullTemplates", {})):
                with self.subTest(path=path):
                    self.assertTrue(any(path.startswith(f"{key}.")
                                        for key in shaped), path)

    def test_the_or_null_template_is_the_rule_struct_as_the_source_declares_it(self):
        found = null_template_declarations()
        self.assertTrue(found)
        for unit, route, path, decl in found:
            source = (ROOT / decl["file"]).read_text()
            declared = struct_fields(source, decl["struct"])
            self.assertIsNotNone(declared, f"no struct {decl['struct']}")
            self.assertEqual(declared, dict(decl["fields"]))


class ClosedMarkerTest(unittest.TestCase):
    def test_an_enum_marker_accepts_only_a_member_of_its_set(self):
        marker = "<VOLATILE:enum:ok|slow|insufficient>"
        self.assertEqual(golden.differences(marker, "slow"), [])
        self.assertEqual(golden.differences(marker, "broken"),
                         [f": '{marker}' -> 'broken'"])
        self.assertEqual(golden.differences(marker, 3),
                         [f": '{marker}' -> 3"])

    def test_an_or_null_template_accepts_null_or_its_declared_fields(self):
        marker = golden.or_null_marker({"profile": "string",
                                        "minCores": "number",
                                        "vectorIsa": "boolean"})
        self.assertEqual(marker, {"<VOLATILE:or-null>": {
            "profile": "<VOLATILE:string>",
            "minCores": "<VOLATILE:number>",
            "vectorIsa": "<VOLATILE:boolean>"}})
        self.assertEqual(golden.differences(marker, None), [])
        rule = {"profile": "quality", "minCores": 4, "vectorIsa": True}
        self.assertEqual(golden.differences(marker, rule), [])
        self.assertEqual(
            golden.differences(marker, {**rule, "minCores": "4"}),
            [".minCores: '<VOLATILE:number>' -> 'string'"])

    def test_a_grammar_enum_marker_needs_a_non_empty_distinct_set(self):
        self.assertEqual(grammar_problems({"x": "<VOLATILE:enum:a|b>"}), [])
        for value in ("<VOLATILE:enum:>", "<VOLATILE:enum:a||b>",
                      "<VOLATILE:enum:a|a>"):
            with self.subTest(value=value):
                self.assertEqual(len(grammar_problems({"x": value})), 1)

    def test_an_or_null_marker_holds_only_a_plain_template(self):
        self.assertEqual(grammar_problems({"x": {"<VOLATILE:or-null>": {
            "a": "<VOLATILE:string>"}}}), [])
        self.assertEqual(len(grammar_problems({"x": {
            "<VOLATILE:or-null>": {"a": "<VOLATILE:string>"},
            "other": 1}})), 1)


class FixtureGrammarTest(unittest.TestCase):
    def test_every_marker_in_every_fixture_belongs_to_the_grammar(self):
        problems = fixture_grammar_problems()
        self.assertEqual(problems, [], "\n".join(problems))

    def test_a_scalar_marker_names_only_json_types_once_and_null_last(self):
        for value in ("<VOLATILE:number>", "<VOLATILE:object|null>",
                      "<VOLATILE:string|number|null>", "<VOLATILE:null>"):
            with self.subTest(value=value):
                self.assertEqual(grammar_problems({"x": value}), [])
        for value in ("<VOLATILE:float>", "<VOLATILE:number|number>",
                      "<VOLATILE:null|string>", "<VOLATILE:>",
                      "<VOLATILE:array>"):
            with self.subTest(value=value):
                problems = grammar_problems({"x": value})
                self.assertEqual(len(problems), 1, problems)

    def test_an_unknown_marker_fails_and_is_named(self):
        problems = grammar_problems({"items": {"<VOLATILE:float>": 1}})
        self.assertIn("$.items: unknown marker key '<VOLATILE:float>'",
                      problems)
        problems = grammar_problems({"x": "<VOLATILE:uuid>"})
        self.assertEqual(problems, ["$.x: unknown marker '<VOLATILE:uuid>'"])
        problems = grammar_problems({"x": "<MYSTERY>"})
        self.assertEqual(problems, ["$.x: unknown marker '<MYSTERY>'"])

    def test_a_volatile_array_takes_an_element_template_or_null(self):
        template = {"<VOLATILE:array>": {"id": "<VOLATILE:number>",
                                         "note": "<VOLATILE:string|null>"},
                    "<VOLATILE:mayBeEmpty>": True}
        self.assertEqual(grammar_problems({"items": template}), [])
        self.assertEqual(grammar_problems({"items": {
            "<VOLATILE:array>": None}}), [])
        problems = grammar_problems({"items": {
            "<VOLATILE:array>": {"id": "<VOLATILE:number>"},
            "<VOLATILE:mayBeEmpty>": False}})
        self.assertEqual(problems, ["$.items: <VOLATILE:mayBeEmpty> must be "
                                    "true"])
        problems = grammar_problems({"items": {
            "<VOLATILE:array>": {"id": "<VOLATILE:float>"}}})
        self.assertEqual(problems, ["$.items[].id: unknown marker "
                                    "'<VOLATILE:float>'"])

    def test_a_plain_or_normalization_marker_is_in_the_grammar(self):
        body = {"a": "<GENERATED>", "b": "<BYTES>", "c": "<masked:string>",
                "d": "<masked-hex32>", "e": "<STACK>", "f": "free text"}
        self.assertEqual(grammar_problems(body), [])


if __name__ == "__main__":
    unittest.main()
