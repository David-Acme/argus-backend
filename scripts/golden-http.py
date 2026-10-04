#!/usr/bin/env python3
import argparse
import json
import re
import ssl
import struct
import subprocess
import sys
import urllib.error
import urllib.request
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
RECORDER_UA = "argus-golden-recorder/1.0"
OTHER_UA = "argus-golden-other-device/1.0"
MISSING_ID = "999999"
MISSING_TOKEN = "argus-golden-missing-capability"
BOUNDARY = "argusgoldenboundary"
SERVICE_REFRESH = "/auth/refresh-token"

EXPECTATIONS = {
    "no-token": 401,
    "other-device": 401,
    "bad-json": 400,
}

MASKED_KEYS = (
    "accessToken",
    "refreshToken",
    "token",
    "capability",
    "expiresAt",
    "expiresIn",
    "uptimeSeconds",
    "serverTime",
)

UNMASKED_UNDER = {"errors"}

ENVELOPE_KEYS = {"status", "info", "errors"}

CONTROL_ERROR_CODE = "CAMERA_UNREACHABLE"

BODY_OVERRIDES = {
    ("auth", "PATCH", "/auth/me"): json.dumps({"name": "p" * 125}),
    ("identity", "POST", "/invitation"): json.dumps({"role": "owner"}),
    ("guard", "POST", "/guard/mode"): json.dumps(
        {"mode": "argus-probe", "environmentId": 0}),
    ("settings", "PATCH", "/settings/{1}"): json.dumps(
        {"changes": [{"key": "probe.setting", "value": "argus-probe"}]}),
}

CONTROL_ROUTES = (
    "capabilities",
    "preset",
    "presets",
    "ptz",
    "settings",
    "snapshot",
    "status",
    "talk",
)

BODY_METHODS = ("POST", "PATCH", "PUT")

SCOPED_PROBES = {
    ("auth", "DELETE", "/auth/sessions"): (
        {"name": "resident-scope-others", "auth": "resident",
         "query": "?scope=others"},
        {"name": "guest-scope-all", "auth": "guest", "query": "?scope=all"},
    ),
}

ID_SLOTS = {
    "/auth/device-login/": MISSING_ID,
    "/auth/users/": "ownerUser",
    "/camera/": "camera",
    "/zone/": "zone",
    "/invitation/": MISSING_ID,
    "/user/": MISSING_ID,
    "/portrait-preview/": "ownerUser",
    "/calendar-event-share/": MISSING_ID,
    "/calendar-event/": "calendarEvent",
    "/project-member/": "projectMember",
    "/project-task/": "projectTask",
    "/project/": "project",
    "/guard/": MISSING_ID,
}

SESSION_KILLERS = {
    ("auth", "PATCH", "/auth/logout"),
    ("auth", "DELETE", "/auth/sessions"),
}

VOLATILE_FIELDS = {
    ("notification", "/notification/delivery-summary"): {
        "keys": ("latencyMsMax", "latencyMsP50", "latencyMsP95",
                 "probeMs", "probeOk"),
        "reason": "the push channel's self-test runs on its own schedule, so "
                  "these fields report the last probe's outcome rather than a "
                  "contract; the counters beside them stay pinned",
    },
    ("camera", "/camera/overview"): {
        "keys": ("cameras[].health",),
        "reason": "a camera's health is the stream supervisor's last "
                  "observation, unknown until its first probe and unreachable "
                  "after it for the fixture camera; the rest of each row stays "
                  "pinned",
    },
    ("settings", "/settings/profiles"): {
        "keys": ("recommendation",),
        "reason": "the recommendation is derived from the cores, RAM, ISA "
                  "and GPU of the host that answers, so it differs between "
                  "machines; the profile previews beside it stay pinned",
    },
}

PORT_RE = re.compile(r"^port\s*=\s*(\d+)\s*$", re.MULTILINE)
PLAIN_RE = re.compile(r"^plain\s*=\s*(true|false)\s*$", re.MULTILINE)
TOKEN_RE = re.compile(r"eyJ[A-Za-z0-9_\-]+\.[A-Za-z0-9_\-]+\.[A-Za-z0-9_\-]+")
HEX64_RE = re.compile(r"\b[0-9a-fA-F]{64}\b")
HEX32_RE = re.compile(r"\b[0-9a-f]{32}\b")

CENSUS_IDS = {
    "camera": 1,
    "zone": 1,
    "project": 1,
    "projectMember": 1,
    "projectTask": 1,
    "calendarEvent": 1,
    "ownerUser": 1,
}

TEXT_LIMIT = 400


def gray_png(size=128):
    def chunk(tag, data):
        body = tag + data
        crc = struct.pack(">I", zlib.crc32(body))
        return struct.pack(">I", len(data)) + body + crc

    rows = []
    for y in range(size):
        row = bytearray(b"\x00")
        for x in range(size):
            value = (x * 3 + y * 5) % 256
            row += bytes((value, value, value))
        rows.append(bytes(row))
    header = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
            + chunk(b"IEND", b""))


def multipart(parts):
    chunks = []
    for name, filename, content_type, data in parts:
        head = f'--{BOUNDARY}\r\nContent-Disposition: form-data; name="{name}"'
        if filename:
            head += f'; filename="{filename}"'
        head += "\r\n"
        if content_type:
            head += f"Content-Type: {content_type}\r\n"
        chunks.append(head.encode() + b"\r\n" + data + b"\r\n")
    chunks.append(f"--{BOUNDARY}--\r\n".encode())
    return b"".join(chunks)


def image_png():
    return gray_png()


def parse_baseline(path):
    routes = []
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        unit, methods, route, filters, multipart_flag, source = line.split("\t")
        routes.append({
            "unit": unit,
            "methods": methods.split("/"),
            "path": route,
            "filters": [] if filters == "-" else filters.split(","),
            "multipart": multipart_flag == "multipart",
            "file": source,
        })
    return routes


def stack_base(stack, unit):
    config_path = stack / unit / "config.toml"
    if not config_path.is_file():
        return None
    text = config_path.read_text()
    port = PORT_RE.search(text)
    if not port:
        return None
    plain = PLAIN_RE.search(text)
    scheme = "http" if plain and plain.group(1) == "true" else "https"
    return f"{scheme}://127.0.0.1:{port.group(1)}"


def run_seeder(stack, extra):
    result = subprocess.run(
        [sys.executable, str(REPO_ROOT / "scripts/seed-golden.py"),
         "--stack-dir", str(stack)] + extra,
        capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit("seed-golden.py " + " ".join(extra)
                         + " failed:\n" + result.stdout + result.stderr)


def load_sessions(stack, mint):
    session_path = stack / "seed.json"
    if mint:
        run_seeder(stack, ["--roles"])
    if not session_path.is_file():
        raise SystemExit(f"missing {session_path}; run "
                         "scripts/seed-golden.py --stack-dir " + str(stack))
    seed = json.loads(session_path.read_text())
    return seed["ids"], seed["sessions"]


def clear_sessions(stack):
    run_seeder(stack, ["--roles-clear"])


def session_killed(entry, record):
    return (entry["unit"], entry["method"], entry["route"]) in SESSION_KILLERS \
        and 200 <= record["response"]["status"] < 300


def remint_sessions(stack, auth_base, timeout):
    run_seeder(stack, ["--roles"])
    refresh = json.loads((stack / "seed.json").read_text())["sessions"]
    tokens = {}
    for role, value in refresh.items():
        access, _ = exchange(auth_base, value, timeout)
        tokens[role] = access
    return tokens


def exchange(auth_base, refresh_token, timeout):
    status, _, body = send(auth_base, "PATCH", SERVICE_REFRESH,
                           {"User-Agent": RECORDER_UA},
                           json.dumps({"refreshToken": refresh_token}),
                           "application/json", timeout)
    if status != 200:
        raise SystemExit(f"{SERVICE_REFRESH} refused the seeded session: "
                         f"{status} {body[:200]!r}")
    info = json.loads(body)["info"]
    return info["accessToken"], info["refreshToken"]


def send(base, method, path, headers, body, content_type, timeout):
    data = None
    if body is not None:
        data = body if isinstance(body, bytes) else body.encode()
    request = urllib.request.Request(base + path, data=data, method=method)
    for key, value in headers.items():
        request.add_header(key, value)
    if data is not None and content_type:
        request.add_header("Content-Type", content_type)
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    try:
        with urllib.request.urlopen(request, timeout=timeout,
                                    context=context) as response:
            return (response.status,
                    response.headers.get("Content-Type", ""),
                    response.read())
    except urllib.error.HTTPError as error:
        return (error.code, error.headers.get("Content-Type", ""),
                error.read())
    except urllib.error.URLError as error:
        raise SystemExit(f"{method} {path} unreachable: {error.reason}")


def looks_like_token(value):
    if not isinstance(value, str) or value.count(".") != 2:
        return False
    return len(value) > 40 and all(part for part in value.split("."))


def looks_like_hex64(value):
    return isinstance(value, str) and HEX64_RE.fullmatch(value) is not None


def looks_like_hex32(value):
    return isinstance(value, str) and HEX32_RE.fullmatch(value) is not None


def mask_text(text):
    text = HEX64_RE.sub("<masked-hex64>", TOKEN_RE.sub("<masked-token>", text))
    return HEX32_RE.sub("<masked-hex32>", text)


def mask(value):
    if isinstance(value, dict):
        return {key: mask(item) for key, item in value.items()}
    if isinstance(value, list):
        return [mask(item) for item in value]
    return "<masked>"


def secret_key(key):
    return key in MASKED_KEYS or key.endswith("At")


def normalize(value, masking=True):
    if isinstance(value, dict):
        return {
            key: (mask(item)
                  if masking and secret_key(key)
                  else normalize(item, masking and key not in UNMASKED_UNDER))
            for key, item in value.items()
        }
    if isinstance(value, list):
        return [normalize(item, masking) for item in value]
    if looks_like_token(value):
        return "<masked-token>"
    if looks_like_hex64(value):
        return "<masked-hex64>"
    if looks_like_hex32(value):
        return "<masked-hex32>"
    return value


def slot_id(route, ids):
    if route["methods"] == ["DELETE"]:
        return MISSING_ID
    if route["path"].endswith("/content"):
        return MISSING_TOKEN
    for prefix, slot in ID_SLOTS.items():
        if route["path"].startswith(prefix):
            if slot == "ownerUser":
                return str(ids.get("ownerUser") or MISSING_ID)
            return str(ids.get(slot) or MISSING_ID)
    return MISSING_ID


def route_identity(route, ids, missing):
    path = route["path"].replace("{1}", MISSING_ID if missing
                                 else slot_id(route, ids))
    return path.replace("{2}", MISSING_ID)


def is_control(unit, path):
    parts = path.split("/")
    return (unit == "camera" and len(parts) == 4
            and parts[0] == "" and parts[3] in CONTROL_ROUTES)


def probe_plan(route, ids, roles):
    method = route["methods"][0]
    filters = route["filters"]
    guarded = "JwtFilter" in filters
    probes = []

    def add(name, auth=None, ua=RECORDER_UA, body=None,
            content_type="application/json", missing=False, query=""):
        probes.append({
            "name": name,
            "auth": auth,
            "userAgent": ua,
            "body": body,
            "contentType": content_type if body is not None else None,
            "path": route_identity(route, ids, missing) + query,
        })

    if route["multipart"]:
        add("multipart-empty",
            body=multipart([("lang", None, None, b"es")]),
            content_type=f"multipart/form-data; boundary={BOUNDARY}")
        add("multipart-image",
            body=multipart([
                ("lang", None, None, b"es"),
                ("image", "golden.png", "image/png", image_png()),
            ]),
            content_type=f"multipart/form-data; boundary={BOUNDARY}")
        return probes

    if route["methods"] == ["WS"]:
        add("ws-plain-get")
        return probes

    body = BODY_OVERRIDES.get((route["unit"], method, route["path"]))
    if body is None and method in BODY_METHODS:
        body = "{}"
    if guarded:
        add("no-token", body=body)
    if "ValidJsonFilter" in filters and method in BODY_METHODS:
        add("bad-json", body="not-json")
    if guarded and "DeviceFilter" in filters:
        add("other-device", auth="owner", ua=OTHER_UA, body=body)
    if "RoleFilter" in filters:
        for role in roles:
            if role == "owner":
                continue
            add(f"role-{role}", auth=role, body=body)

    if is_control(route["unit"], route["path"]):
        add("owner", auth="owner", body=body)
        add("owner-missing", auth="owner", body=body, missing=True)
        return probes

    if guarded:
        add("owner", auth="owner", body=body)
    else:
        add("plain", body=body)
    if "{1}" in route["path"] and slot_id(route, ids) != MISSING_ID:
        add("owner-missing", auth="owner" if guarded else None, body=body,
            missing=True)
    for scoped in SCOPED_PROBES.get((route["unit"], method, route["path"]), ()):
        if scoped["auth"] in roles:
            add(scoped["name"], auth=scoped["auth"], body=body,
                query=scoped["query"])
    return probes


def build_plan(routes, ids, roles, bases=None):
    plan = []
    booted = sorted(bases or {})
    owned = {(route["unit"], route["methods"][0], route["path"])
             for route in routes if route["unit"] != "shared"}
    for route in routes:
        if route["unit"] == "shared" and route["unit"] not in booted:
            method = route["methods"][0]
            for unit in booted or ["shared"]:
                if (unit, method, route["path"]) in owned:
                    continue
                plan.append({
                    "unit": "shared",
                    "baseUnit": unit,
                    "method": route["methods"][0],
                    "route": route["path"],
                    "filters": route["filters"],
                    "multipart": route["multipart"],
                    "source": route["file"],
                    "probe": {
                        "name": f"plain-{unit}",
                        "auth": None,
                        "userAgent": RECORDER_UA,
                        "body": None,
                        "contentType": None,
                        "path": route["path"],
                    },
                })
            continue
        for probe in probe_plan(route, ids, roles):
            plan.append({
                "unit": route["unit"],
                "baseUnit": route["unit"],
                "method": route["methods"][0],
                "route": route["path"],
                "filters": route["filters"],
                "multipart": route["multipart"],
                "source": route["file"],
                "probe": probe,
            })
    return plan


def plan_routes(routes, bases):
    return [route for route in routes
            if route["unit"] in bases or route["unit"] == "shared"]


def unbooted(routes, bases):
    return sorted({route["unit"] for route in routes} - set(bases)
                  - {"shared"})


def session_of(name, sessions):
    if name is None:
        return None
    if name not in sessions:
        raise SystemExit(f"the seed carries no '{name}' session")
    return sessions[name]


def run_probe(entry, base, sessions, timeout):
    probe = entry["probe"]
    headers = {"User-Agent": probe["userAgent"], "Accept": "application/json"}
    token = session_of(probe["auth"], sessions)
    if token:
        headers["Authorization"] = f"Bearer {token}"
    payload = probe["body"]
    if payload is None:
        described = None
    elif isinstance(payload, bytes):
        described = f"<{len(payload)} bytes>"
    else:
        described = payload
    status, content_type, raw = send(
        base, entry["method"], probe["path"], headers, payload,
        probe["contentType"], timeout)
    body = raw.decode("utf-8", "replace")
    record = {
        "unit": entry["unit"],
        "method": entry["method"],
        "path": probe["path"],
        "route": entry["route"],
        "probe": probe["name"],
        "request": {
            "auth": probe["auth"] or "none",
            "userAgent": "other" if probe["userAgent"] == OTHER_UA
                         else "recorder",
            "contentType": probe["contentType"],
            "body": described,
        },
        "response": {
            "status": status,
            "contentType": content_type.split(";")[0].strip(),
            "pretty": "\n" in body,
        },
    }
    try:
        parsed = json.loads(body)
    except json.JSONDecodeError:
        record["response"]["json"] = None
        record["response"]["text"] = mask_text(body)[:TEXT_LIMIT]
    else:
        record["response"]["json"] = normalize(parsed)
    return record


def error_code(record):
    body = record["response"].get("json")
    if not isinstance(body, dict):
        return None
    errors = body.get("errors")
    if not isinstance(errors, dict):
        return None
    return errors.get("code")


def violates(entry, record, deviations):
    probe = entry["probe"]["name"]
    response = record["response"]
    status = response["status"]
    problems = []
    control = is_control(entry["unit"], entry["route"])
    if status >= 500 and not (status == 502 and control):
        problems.append(f"{status} is a server failure")
    elif control and status == 502 and error_code(record) != CONTROL_ERROR_CODE:
        problems.append(f"control route answered 502 "
                        f"{error_code(record)} instead of {CONTROL_ERROR_CODE}")
    if probe in EXPECTATIONS and status != EXPECTATIONS[probe]:
        problems.append(f"{probe} answered {status}, "
                        f"expected {EXPECTATIONS[probe]}")
    if entry["multipart"] and status == 400:
        problems.append("multipart body was refused with 400")
    if status == 204:
        if response.get("text") or response.get("json") is not None:
            problems.append("204 answered with a body")
    elif response["contentType"] == "application/json" and \
            not declared(entry, deviations):
        keys = set(response["json"]) if isinstance(response["json"], dict) \
            else set()
        if keys != ENVELOPE_KEYS:
            problems.append("body is not the {status, info, errors} envelope "
                            f"but {sorted(keys)}")
    return problems


def declared(entry, deviations):
    return any((item["unit"], item["method"], item["route"]) ==
               (entry["unit"], entry["method"], entry["route"])
               for item in deviations)


def group_by_unit(probes):
    units = {}
    for entry in probes:
        units.setdefault(entry["unit"], []).append(entry)
    return units


def fixture_path(fixtures, unit):
    return fixtures / f"{unit}.json"


def load_manifest(fixtures):
    path = fixtures / "manifest.json"
    if not path.is_file():
        return {"declaredDeviations": []}
    return json.loads(path.read_text())


def write_manifest(fixtures, units, probes, deviations):
    manifest = {
        "fixtures": "scripts/fixtures/http",
        "sources": "scripts/lib/route-baseline.txt (the route census)",
        "harness": "scripts/golden-http.py",
        "sessions": "scripts/seed-golden.py --roles (owner, resident, guest)",
        "normalization": {
            "maskedKeys": list(MASKED_KEYS),
            "maskedSuffix": "*At",
            "maskedTokens": "any value shaped like a three-part JWT",
            "maskedHex64": "any 64-character hexadecimal value",
            "maskedHex32": "any 32-character lowercase hexadecimal value "
                           "(a session id)",
            "unmaskedUnder": sorted(UNMASKED_UNDER),
            "maskedShape": "a masked key keeps the shape of what it held, so "
                           "an object under it is still compared field by field",
        },
        "expectations": EXPECTATIONS,
        "volatileFields": [
            {"unit": unit, "route": route, "keys": list(fields["keys"]),
             "reason": fields["reason"]}
            for (unit, route), fields in sorted(VOLATILE_FIELDS.items())
        ],
        "invariants": [
            "no probe may answer 5xx except the camera-control 502, which "
            "must carry CAMERA_UNREACHABLE",
            "a probe with no token must answer 401, and so must a probe "
            "whose token belongs to another device",
            "a malformed JSON body on a ValidJsonFilter route "
            "must answer 400",
            "a multipart body must never be answered 400",
            "a JSON answer must carry exactly the "
            "{status, info, errors} envelope",
            "a 204 answer must carry an empty body",
        ],
        "declaredDeviations": deviations,
        "units": units,
        "probes": len(probes),
    }
    (fixtures / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")


def recorded_units(fixtures):
    return sorted(path.stem for path in fixtures.glob("*.json")
                  if path.stem != "manifest")


def dropped_units(fixtures, bases):
    return sorted(unit for unit in recorded_units(fixtures)
                  if unit != "shared" and unit not in bases)


def record(fixtures, routes, bases, sessions, ids, roles, timeout, stack):
    unknown = unbooted(routes, bases)
    for unit in unknown:
        print(f"skip: {unit} is not booted in this stack")
    dropped = dropped_units(fixtures, bases)
    if dropped:
        print("refusing to record: the stack is missing the units "
              + ", ".join(dropped) + ", whose recordings would be dropped",
              file=sys.stderr)
        return 2
    orphans = sorted(unit for unit in recorded_units(fixtures)
                     if unit not in {route["unit"] for route in routes})
    if orphans:
        print("refusing to record: " + ", ".join(orphans) + " have recordings "
              "but no route in the census; delete them or restore the routes",
              file=sys.stderr)
        return 2
    manifest = load_manifest(fixtures)
    deviations = manifest["declaredDeviations"]
    plan = build_plan(plan_routes(routes, bases), ids, roles, bases)
    failures = 0
    units = {}
    fixtures.mkdir(parents=True, exist_ok=True)
    for entry in plan:
        record_entry = run_probe(entry, bases[entry["baseUnit"]], sessions,
                                timeout)
        problems = violates(entry, record_entry, deviations)
        for problem in problems:
            failures += 1
            print(f"violation: {entry['unit']} {entry['method']} "
                  f"{entry['route']} [{entry['probe']['name']}] {problem}",
                  file=sys.stderr)
        units.setdefault(entry["unit"], []).append(record_entry)
        if session_killed(entry, record_entry):
            sessions = remint_sessions(stack, bases["auth"], timeout)
    for unit, records in sorted(units.items()):
        fixture_path(fixtures, unit).write_text(
            json.dumps({"unit": unit, "probes": records},
                       indent=2, sort_keys=True) + "\n")
    write_manifest(fixtures, sorted(units), plan, deviations)
    print(f"record: {len(plan)} probes over {len(units)} units, "
          f"{failures} violations, {len(unknown)} units skipped")
    return 1 if failures else 0


def load_fixture(fixtures, unit):
    path = fixture_path(fixtures, unit)
    if not path.is_file():
        return {}
    data = json.loads(path.read_text())
    return {(item["method"], item["path"], item["probe"]): item
            for item in data["probes"]}


def differences(expected, actual, prefix=""):
    if isinstance(expected, dict) and isinstance(actual, dict):
        found = []
        for key in sorted(set(expected) | set(actual)):
            if key not in expected:
                found.append(f"{prefix}.{key} appeared")
            elif key not in actual:
                found.append(f"{prefix}.{key} disappeared")
            else:
                found.extend(differences(expected[key], actual[key],
                                         f"{prefix}.{key}"))
        return found
    if isinstance(expected, list) and isinstance(actual, list):
        if len(expected) != len(actual):
            return [f"{prefix}: {len(expected)} -> {len(actual)} items"]
        found = []
        for index, (left, right) in enumerate(zip(expected, actual)):
            found.extend(differences(left, right, f"{prefix}[{index}]"))
        return found
    if isinstance(expected, bool) != isinstance(actual, bool):
        return [f"{prefix}: {expected!r} -> {actual!r}"]
    if expected != actual:
        return [f"{prefix}: {expected!r} -> {actual!r}"]
    return []


def volatile_of(manifest):
    entries = manifest.get("volatileFields")
    if entries is None:
        return VOLATILE_FIELDS
    return {(entry["unit"], entry["route"]): {"keys": tuple(entry["keys"]),
                                              "reason": entry.get("reason", "")}
            for entry in entries}


def without_volatile(entry, body, volatile):
    fields = volatile.get((entry["unit"], entry["route"]))
    if not fields or not isinstance(body, dict):
        return body
    info = body.get("info")
    if not isinstance(info, dict):
        return body
    body = json.loads(json.dumps(body))
    for key in fields["keys"]:
        drop_path(body["info"], key.split("."))
    return body


def drop_path(node, parts):
    head, rest = parts[0], parts[1:]
    if head.endswith("[]"):
        items = node.get(head[:-2]) if isinstance(node, dict) else None
        for item in items if isinstance(items, list) else ():
            drop_path(item, rest)
        return
    if not isinstance(node, dict):
        return
    if rest:
        drop_path(node.get(head), rest)
    else:
        node.pop(head, None)


def compare(expected, actual, volatile):
    problems = []
    for field in ("status", "contentType", "pretty"):
        if expected["response"][field] != actual["response"][field]:
            problems.append(
                f"response.{field}: {expected['response'][field]!r} -> "
                f"{actual['response'][field]!r}")
    problems.extend(differences(
        without_volatile(expected, expected["response"].get("json"), volatile),
        without_volatile(actual, actual["response"].get("json"), volatile),
        "response.json"))
    if expected["response"].get("text") != actual["response"].get("text"):
        problems.append("response.text changed")
    return problems


def verify(fixtures, routes, bases, sessions, ids, roles, timeout, verbose,
           stack):
    for unit in unbooted(routes, bases):
        print(f"skip: {unit} is not booted in this stack")
    manifest = load_manifest(fixtures)
    deviations = manifest["declaredDeviations"]
    volatile = volatile_of(manifest)
    plan = build_plan(plan_routes(routes, bases), ids, roles, bases)
    expected_by_unit = {unit: load_fixture(fixtures, unit)
                        for unit in {entry["unit"] for entry in plan}}
    checked = 0
    failures = 0
    for entry in plan:
        fixture = expected_by_unit[entry["unit"]]
        key = (entry["method"], entry["probe"]["path"], entry["probe"]["name"])
        expected = fixture.get(key)
        if expected is None:
            failures += 1
            print(f"unrecorded: {entry['unit']} {entry['method']} "
                  f"{entry['route']} [{entry['probe']['name']}]",
                  file=sys.stderr)
            continue
        actual = run_probe(entry, bases[entry["baseUnit"]], sessions, timeout)
        problems = compare(expected, actual, volatile)
        problems += violates(entry, actual, deviations)
        checked += 1
        if problems:
            failures += 1
            print(f"drift: {entry['unit']} {entry['method']} {entry['route']}"
                  f" [{entry['probe']['name']}]", file=sys.stderr)
            for problem in problems:
                print(f"  {problem}", file=sys.stderr)
        elif verbose:
            print(f"ok: {entry['unit']} {entry['method']} {entry['route']} "
                  f"[{entry['probe']['name']}] "
                  f"{actual['response']['status']}")
        if session_killed(entry, actual):
            sessions = remint_sessions(stack, bases["auth"], timeout)
    stale = 0
    unverified = 0
    for unit, fixture in expected_by_unit.items():
        live = {(entry["method"], entry["probe"]["path"],
                 entry["probe"]["name"])
                for entry in plan if entry["unit"] == unit}
        for key in sorted(set(fixture) - live):
            stale += 1
            print(f"stale: {unit} {key[0]} {key[1]} [{key[2]}] "
                  "no longer in the census", file=sys.stderr)
    censused = {route["unit"] for route in routes}
    for path in sorted(fixtures.glob("*.json")):
        if path.stem == "manifest" or path.stem in censused:
            continue
        stale += 1
        print(f"stale: {path.stem} has fixtures but no route in the census",
              file=sys.stderr)
    for unit in dropped_units(fixtures, bases):
        unverified += len(load_fixture(fixtures, unit))
        print(f"unverified: {unit} is not booted, so its "
              f"{len(load_fixture(fixtures, unit))} recordings were not checked",
              file=sys.stderr)
    print(f"verify: {checked} probes checked, {failures} failing, "
          f"{stale} stale, {unverified} probes unverified")
    return 1 if failures or stale or unverified else 0


def census(routes):
    bases = {route["unit"]: "" for route in routes}
    plan = build_plan(routes, dict(CENSUS_IDS), ["owner", "resident", "guest"],
                      bases)
    for unit, entries in sorted(group_by_unit(plan).items()):
        print(f"{unit}\t{len(entries)} probes\t"
              f"{len({entry['route'] for entry in entries})} routes")
    print(f"census: {len(plan)} probes over {len(group_by_unit(plan))} units, "
          "planned against every unit the census declares")
    return 0


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Replay the golden HTTP contracts of every route "
                    "the census declares against a booted fleet.")
    parser.add_argument("command", choices=("record", "verify", "census"))
    parser.add_argument("--stack-dir",
                        default=str(REPO_ROOT / "build/native-stack"))
    parser.add_argument("--fixtures",
                        default=str(REPO_ROOT / "scripts/fixtures/http"))
    parser.add_argument("--baseline",
                        default=str(REPO_ROOT / "scripts/lib/route-baseline.txt"))
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--no-mint", action="store_true",
                        help="reuse the sessions already in the stack")
    parser.add_argument("--seed", action="store_true",
                        help="seed the golden fixture rows first; a stack that "
                             "has none answers 404 where the fixture rows are "
                             "the subject")
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    routes = parse_baseline(Path(args.baseline))
    if args.command == "census":
        return census(routes)

    stack = Path(args.stack_dir)
    fixtures = Path(args.fixtures)
    bases = {}
    for unit in sorted({route["unit"] for route in routes}):
        base = stack_base(stack, unit)
        if base:
            bases[unit] = base
    if not bases:
        print("no service of the census is booted; run "
              "scripts/native-stack.sh up", file=sys.stderr)
        return 1

    if "auth" not in bases:
        print("the census harness needs argus-auth booted to mint sessions",
              file=sys.stderr)
        return 1

    if args.seed:
        run_seeder(stack, [])
    ids, refresh = load_sessions(stack, not args.no_mint)
    absent = sorted(slot for slot, value in ids.items() if not value)
    if absent:
        print("the stack has no fixture row for " + ", ".join(absent)
              + "; pass --seed or run scripts/seed-golden.py --stack-dir "
              + str(stack), file=sys.stderr)
        return 1
    roles = [name for name in ("owner", "resident", "guest") if name in refresh]
    missing = [name for name in ("owner", "resident", "guest")
               if name not in refresh]
    if missing:
        print("warning: no session for " + ", ".join(missing)
              + "; the role matrix is not probed", file=sys.stderr)
    sessions = {}
    for role in roles:
        access, _ = exchange(bases["auth"], refresh[role], args.timeout)
        sessions[role] = access
    try:
        if args.command == "record":
            return record(fixtures, routes, bases, sessions, ids, roles,
                          args.timeout, stack)
        return verify(fixtures, routes, bases, sessions, ids, roles,
                      args.timeout, args.verbose, stack)
    finally:
        if not args.no_mint:
            clear_sessions(stack)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
