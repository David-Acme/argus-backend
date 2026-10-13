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


def run_harness(command, fixtures, fleet):
    output = io.StringIO()
    with mock.patch.object(golden, "send", fleet.send), \
            contextlib.redirect_stdout(output), \
            contextlib.redirect_stderr(output):
        if command == "record":
            code = golden.record(fixtures, health_routes(), BASES, {},
                                 dict(golden.CENSUS_IDS), [], 5.0,
                                 fixtures.parent)
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
        actual = answered(guard, "top", "recommendation", "balanced")
        self.assertEqual(compared(guard, actual),
                         ["response.json.recommendation appeared"])

    def test_the_top_level_drop_never_reaches_an_enveloped_body(self):
        enveloped = recorded("shared", "/health", "plain-auth")
        actual = answered(enveloped, "top", "internalTls", LOADED)
        self.assertEqual(compared(enveloped, actual),
                         ["response.json.internalTls appeared"])

    def test_the_drop_is_declared_per_unit_and_route(self):
        guard = recorded("guard", "/health", "plain")
        elsewhere = copy.deepcopy(guard)
        elsewhere["route"] = "/guard/safety"
        actual = answered(elsewhere, "top", "internalTls", LOADED)
        self.assertEqual(compared(elsewhere, actual),
                         ["response.json.internalTls appeared"])

        other_unit = copy.deepcopy(guard)
        other_unit["unit"] = "camera"
        actual = answered(other_unit, "top", "internalTls", LOADED)
        self.assertEqual(compared(other_unit, actual),
                         ["response.json.internalTls appeared"])


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
                self.assertNotIn("internalTls",
                                 (self.fixtures / name).read_text())
        manifest = json.loads((self.fixtures / "manifest.json").read_text())
        manifest.pop("volatileFields")
        self.assertNotIn("internalTls", json.dumps(manifest))

        shared = json.loads((self.fixtures / "shared.json").read_text())
        self.assertEqual([probe["probe"] for probe in shared["probes"]],
                         ["plain-auth"])
        self.assertEqual(shared["probes"][0]["response"]["json"]["info"],
                         {"service": "argus-auth",
                          "uptimeSeconds": "<masked>"})
        guard = json.loads((self.fixtures / "guard.json").read_text())
        self.assertEqual(guard["probes"][0]["response"]["json"],
                         {"status": "ok", "service": "argus-guard"})

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


if __name__ == "__main__":
    unittest.main()
