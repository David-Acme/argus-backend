#!/usr/bin/env python3
import argparse
import importlib.util
import json
import re
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

SERVICE_TYPE = "_argus-route._tcp.local"
NOTHING_TYPE = "_argus-nothing._tcp.local"
META_QUERY = "_services._dns-sd._udp.local"
MDNS_ADDRESS = "224.0.0.251"
MDNS_PORT = 5353
BROWSE_SECONDS = 3.0
TYPE_A = 1
TYPE_PTR = 12
TYPE_TXT = 16
TYPE_AAAA = 28
TYPE_SRV = 33
CLASS_IN = 1
CLASS_UNICAST = 0x8000

SANDBOX_UNITS = ("identity", "auth", "camera", "productivity",
                 "notification", "sync", "guard")
CLIENT_ROUTES = ("auth", "sync", "media")
ROUTE_UNITS = {
    "auth": "auth",
    "sync": "sync",
    "camera": "camera",
    "media": "camera",
    "zone": "camera",
    "invitation": "identity",
    "pairing": "identity",
    "portrait-preview": "identity",
    "user": "identity",
    "guard": "guard",
    "notification": "notification",
    "notification-token": "notification",
    "calendar-event": "productivity",
    "calendar-event-share": "productivity",
    "project": "productivity",
    "project-member": "productivity",
    "project-task": "productivity",
}

NEW_DEVICE_UA = "argus-discovery-drill/1.0"
BOOTSTRAP_TABLES = ("user", "camera", "person", "notification")
CAMERA_ID = 1
GOLDEN_USER = {"id": 1, "role": "owner"}
GOLDEN_CAMERA = {"id": 1, "name": "Golden Cam"}
WAV_FIXTURE = REPO_ROOT / "models/stt/zipformer-en/test_wavs/0.wav"
PCM_CHUNK_SAMPLES = 3200
GREETING_SECONDS = 40.0
GREETING_QUIET_SECONDS = 1.5
TURN_SECONDS = 120.0

WATERMARK_REQUEST = json.dumps({"type": "sync_audit_log",
                                "payload": {"findLast": True}})
BOOTSTRAP_REQUEST = json.dumps({
    "type": "sync",
    "payload": {table: {"findLastCreated": True, "findLastDeleted": True,
                        "requiredCreate": True, "requiredDeleted": True}
                for table in BOOTSTRAP_TABLES}})
VOICE_START = json.dumps({"type": "voice:start", "payload": {}})
VOICE_STOP = json.dumps({"type": "voice:stop", "payload": {}})
MEDIA_SUBSCRIBE = json.dumps({"type": "camera:subscribe",
                              "payload": {"cameraId": CAMERA_ID,
                                          "quality": "main"}})

MODULES = {}
SKIPPED = []


def usage():
    return (
        "discovery-drill: browse _argus-route._tcp for this host's fleet\n"
        "and drive a real client through the endpoints it resolved, never\n"
        "through a port written down in advance.\n"
        "\n"
        "  resolve    browse, and check every announcement against the\n"
        "             sandbox it came from\n"
        "  login      discover auth, then log a new device in through it\n"
        "  bootstrap  discover sync and bootstrap its tables over it\n"
        "  media      discover the camera and subscribe to its media socket\n"
        "  voice      discover sync and drive a voice turn over it\n"
        "  all        every leg above, in order, on one discovery\n"
        "  state      what is advertised and what is up, without driving\n"
        "\n"
        "The sandbox must advertise its routes: boot it with\n"
        "ARGUS_STACK_MDNS=1. The client dials the address the responder\n"
        "announced, so a route only the loopback carries is reported.\n"
    )


def load(name, path):
    if name not in MODULES:
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        MODULES[name] = module
    return MODULES[name]


def durability():
    return load("durability_drill", REPO_ROOT / "scripts/durability-drill.py")


def isolation():
    return load("isolation_drill", REPO_ROOT / "scripts/isolation-drill.py")


def golden():
    return durability().load_golden()


def check(results, name, ok, detail=""):
    durability().check(results, name, ok, detail)


def skip(reason):
    print(f"  skip  {reason}", flush=True)
    SKIPPED.append(reason)


def http(base, method, path, headers, body=None, timeout=25.0):
    started = time.monotonic()
    try:
        status, _, text = golden().send(base, method, path, headers, body,
                                        "application/json", timeout)
    except SystemExit as error:
        return 0, time.monotonic() - started, str(error)
    return status, time.monotonic() - started, text


def body_text(text):
    if isinstance(text, bytes):
        return text.decode("utf-8", "replace")
    return text


def info_of(text):
    try:
        return json.loads(text)["info"]
    except (ValueError, KeyError, TypeError):
        return {}


def encode_name(name):
    out = bytearray()
    for label in name.rstrip(".").split("."):
        out.append(len(label))
        out += label.encode("utf-8")
    out.append(0)
    return bytes(out)


def read_name(packet, offset):
    labels = []
    jumps = 0
    end = None
    while True:
        if offset >= len(packet):
            raise SystemExit("a DNS name ran past the end of the packet")
        length = packet[offset]
        if length == 0:
            offset += 1
            break
        if length & 0xC0 == 0xC0:
            if offset + 1 >= len(packet):
                raise SystemExit("a DNS pointer ran past the packet")
            if end is None:
                end = offset + 2
            offset = ((length & 0x3F) << 8) | packet[offset + 1]
            jumps += 1
            if jumps > 16:
                raise SystemExit("a DNS name pointed at itself")
            continue
        labels.append(packet[offset + 1:offset + 1 + length]
                      .decode("utf-8", "replace"))
        offset += 1 + length
    return ".".join(labels), (end if end is not None else offset)


def parse_txt(data):
    values = {}
    index = 0
    while index < len(data):
        length = data[index]
        entry = data[index + 1:index + 1 + length].decode("utf-8", "replace")
        key, _, value = entry.partition("=")
        values[key] = value
        index += 1 + length
    return values


def parse_packet(packet):
    if len(packet) < 12:
        return []
    _, _, questions, answers, authority, additional = struct.unpack(
        "!HHHHHH", packet[:12])
    offset = 12
    for _ in range(questions):
        _, offset = read_name(packet, offset)
        offset += 4
    records = []
    for count in (answers, authority, additional):
        for _ in range(count):
            name, offset = read_name(packet, offset)
            if offset + 10 > len(packet):
                return records
            rtype, rclass, _, length = struct.unpack(
                "!HHIH", packet[offset:offset + 10])
            offset += 10
            data = packet[offset:offset + length]
            entry = {"name": name, "type": rtype,
                     "cache_flush": bool(rclass & CLASS_UNICAST)}
            if rtype == TYPE_PTR:
                entry["target"], _ = read_name(packet, offset)
            elif rtype == TYPE_SRV:
                _, _, port = struct.unpack("!HHH", data[:6])
                target, _ = read_name(packet, offset + 6)
                entry.update({"port": port, "target": target})
            elif rtype == TYPE_TXT:
                entry["txt"] = parse_txt(data)
            elif rtype == TYPE_A and length == 4:
                entry["address"] = socket.inet_ntop(socket.AF_INET, data)
            elif rtype == TYPE_AAAA and length == 16:
                entry["address"] = socket.inet_ntop(socket.AF_INET6, data)
            records.append(entry)
            offset += length
    return records


def query_bytes(name, rtype):
    header = struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0)
    return header + encode_name(name) + struct.pack(
        "!HH", rtype, CLASS_IN | CLASS_UNICAST)


def multi_query_bytes(names):
    header = struct.pack("!HHHHHH", 0, 0, len(names), 0, 0, 0)
    questions = b"".join(encode_name(name)
                         + struct.pack("!HH", TYPE_PTR,
                                       CLASS_IN | CLASS_UNICAST)
                         for name in names)
    return header + questions


def browse(name=SERVICE_TYPE, seconds=BROWSE_SECONDS, query=None):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.4)
    records = []
    try:
        sock.sendto(query if query is not None else query_bytes(name, TYPE_PTR),
                    (MDNS_ADDRESS, MDNS_PORT))
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                packet, _ = sock.recvfrom(9000)
            except socket.timeout:
                continue
            except OSError:
                break
            records.extend(parse_packet(packet))
    finally:
        sock.close()
    return records


def resolve(records):
    instances = {}
    hosts = {}
    for record in records:
        if record["type"] == TYPE_PTR and record["name"] == SERVICE_TYPE:
            instances.setdefault(record["target"],
                                 {"instance": record["target"]})
        elif record["type"] in (TYPE_A, TYPE_AAAA) and "address" in record:
            hosts.setdefault(record["name"], []).append(record["address"])
        elif record["type"] == TYPE_SRV:
            instances.setdefault(record["name"], {"instance": record["name"]})
    for record in records:
        instance = instances.get(record.get("name"))
        if instance is not None:
            if record["type"] == TYPE_SRV:
                instance["port"] = record["port"]
                instance["target"] = record["target"]
            elif record["type"] == TYPE_TXT:
                instance.setdefault("txt", {}).update(record["txt"] or {})
    for instance in instances.values():
        target = instance.get("target")
        if target:
            instance["addresses"] = list(dict.fromkeys(hosts.get(target, [])))
    return {name: instance for name, instance in instances.items()
            if instance.get("port")}


def announced_routes(instances):
    table = {}
    for instance in instances.values():
        txt = instance.get("txt") or {}
        path = txt.get("path")
        if not path:
            continue
        table[path.lstrip("/")] = {
            "path": path,
            "request_path": "/" + path.strip("/"),
            "port": instance.get("port"),
            "tls": txt.get("https") == "true",
            "target": instance.get("target"),
            "addresses": instance.get("addresses", []),
            "instance": instance["instance"],
        }
    return table


def configured_base(stack, unit):
    return golden().stack_base(stack, unit)


def configured_port(stack, unit):
    base = configured_base(stack, unit)
    return None if base is None else int(base.rsplit(":", 1)[1])


def configured_tls(stack, unit):
    base = configured_base(stack, unit)
    return base is not None and base.startswith("https://")


def config_value(stack, unit, section, key):
    config = stack / unit / "config.toml"
    if not config.is_file():
        return None
    current = None
    for line in config.read_text().splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            current = stripped[1:-1].strip()
            continue
        if current != section:
            continue
        match = re.match(rf'{key}\s*=\s*(?:"([^"]*)"|(\S+))', stripped)
        if match:
            quoted, bare = match.group(1), match.group(2)
            return quoted if quoted is not None else bare
    return None


def responder_default_name():
    source = (REPO_ROOT / "packages/lib/mdns/src/mdns/mdns-service.cc"
              ).read_text()
    match = re.search(r'kDefaultName\s*=\s*"([^"]*)"', source)
    if match is None:
        raise SystemExit("packages/lib/mdns/src/mdns/mdns-service.cc no "
                         "longer declares the responder's default name")
    return match.group(1)


def advertised_name(stack, unit):
    return (config_value(stack, unit, "mdns", "name")
            or responder_default_name()).replace(".", "-")


def expected_label(declared, segment):
    suffix = "-" + segment.replace(".", "-")
    room = 63 - len(suffix) if len(suffix) < 63 else 0
    return declared[:room] + suffix + "." + SERVICE_TYPE


def host_addresses():
    result = subprocess.run(["ip", "-o", "addr", "show"], capture_output=True,
                            text=True)
    return {match.group(1) for match in
            re.finditer(r"inet6? (\S+)/", result.stdout)}


def announced_address(route):
    for address in route["addresses"]:
        if address not in ("127.0.0.1", "::1"):
            return address
    return route["addresses"][0] if route["addresses"] else None


def endpoint(route, path=""):
    address = announced_address(route)
    if address is None:
        raise SystemExit(f"{route['instance']} carries no address record")
    scheme = "https" if route["tls"] else "http"
    return f"{scheme}://{address}:{route['port']}{path}"


def advertising_off(stack):
    disabled = []
    for unit in SANDBOX_UNITS:
        value = config_value(stack, unit, "mdns", "enabled")
        if value is not None and value.strip().lower() == "false":
            disabled.append(unit)
    return disabled


def expected_routes():
    baseline = REPO_ROOT / "scripts/lib/route-baseline.txt"
    if not baseline.is_file():
        return None
    routes = {}
    for line in baseline.read_text().splitlines():
        fields = line.split("\t")
        if len(fields) < 3:
            continue
        unit, path = fields[0], fields[2]
        if path == "/health":
            continue
        routes.setdefault(unit, set()).add(path.split("/")[1])
    return {unit: sorted(routes.get(unit, set())) for unit in SANDBOX_UNITS}


def duplicate_paths(instances):
    seen = {}
    for instance in instances.values():
        path = (instance.get("txt") or {}).get("path")
        if path:
            key = path.strip("/")
            seen.setdefault(key, []).append(instance["instance"])
    return sorted(key for key, names in seen.items() if len(names) > 1)


def announced_labels(records):
    return {record["target"] for record in records
            if record["type"] == TYPE_PTR and record["name"] == SERVICE_TYPE}


def discover(results, stack, expected=CLIENT_ROUTES):
    records = browse()
    instances = resolve(records)
    table = announced_routes(instances)
    if not table:
        disabled = advertising_off(stack)
        hint = ("mDNS is off in " + ", ".join(disabled)
                + "; boot the sandbox with ARGUS_STACK_MDNS=1") if disabled \
            else "nothing on this host answers " + SERVICE_TYPE
        raise SystemExit("no announcements: " + hint)
    twins = duplicate_paths(instances)
    check(results, "no two instances claim one route path", not twins,
          ", ".join(twins) if twins else f"{len(table)} route(s)")
    labels = announced_labels(records)
    resolved = {instance["instance"] for instance in instances.values()
                if instance.get("port")
                and (instance.get("txt") or {}).get("path")}
    unresolved = sorted(labels - resolved)
    check(results, "every announced instance resolves to a port and a path",
          not unresolved,
          f"{len(resolved)} of {len(labels)} resolved"
          + (f", unresolved: {', '.join(unresolved)}" if unresolved else ""))
    by_label = {}
    for name, route in table.items():
        by_label.setdefault(route["instance"], []).append(name)
    shared = sorted(f"{sorted(names)} on {label}"
                    for label, names in by_label.items() if len(names) > 1)
    check(results, "no two routes are announced under one record name",
          bool(table) and not shared,
          "; ".join(shared) if shared
          else f"{len(table)} route(s) on {len(by_label)} name(s)")

    missing = sorted(set(expected) - set(table))
    check(results, "every route this client needs is announced", not missing,
          "missing: " + ", ".join(missing) if missing
          else ", ".join(sorted(table)))

    strangers = [record for record in browse(NOTHING_TYPE, 1.5)
                 if record["type"] == TYPE_PTR]
    check(results, "a service type nobody serves answers nothing",
          not strangers, NOTHING_TYPE)

    both = {record["name"] for record
            in browse(query=multi_query_bytes((SERVICE_TYPE, META_QUERY)),
                      seconds=2.0)
            if record["type"] == TYPE_PTR}
    check(results, "a packet asking two questions is answered for both",
          SERVICE_TYPE in both and META_QUERY in both,
          f"answered for {sorted(both) or 'nothing'}")

    host = host_addresses()
    for name in sorted(table):
        route = table[name]
        unit = ROUTE_UNITS.get(name)
        if unit is None:
            print(f"  note  /{name} is not a route of this sandbox: "
                  f"{route['instance']}")
            continue
        check(results, f"/{name} announces the port {unit} bound",
              route["port"] == configured_port(stack, unit),
              f"announced {route['port']}, {unit} listens on "
              f"{configured_port(stack, unit)}")
        check(results, f"/{name} announces an address of this host",
              any(address in host for address in route["addresses"]),
              ", ".join(route["addresses"]) or "no address record")
        check(results, f"/{name} names its path and its TLS in TXT",
              route["path"] == name
              and route["tls"] == configured_tls(stack, unit),
              f"path={route['path']!r} https={route['tls']}, {unit} TLS "
              f"{configured_tls(stack, unit)}")
        check(results, f"/{name} is announced under the contract's label",
              route["instance"] == expected_label(advertised_name(stack, unit),
                                                  name),
              f"{route['instance']!r} for host "
              f"{advertised_name(stack, unit)!r} and route {name!r}")

    for name in sorted(set(expected) & set(table)):
        route = table[name]
        check(results, f"the client can address /{name} off the loopback",
              any(address not in ("127.0.0.1", "::1")
                  for address in route["addresses"]),
              f"announced {', '.join(route['addresses']) or 'no address'}")

    wanted = expected_routes()
    check(results, "the route baseline is there to check the fleet against",
          wanted is not None, "scripts/lib/route-baseline.txt")
    for unit, routes in (wanted or {}).items():
        if not routes:
            continue
        announced = sorted(name for name in table
                           if ROUTE_UNITS.get(name) == unit)
        check(results, f"{unit} announces every route the tree declares",
              announced == routes,
              f"announced {announced} of {routes}")
    return table


def session_headers(access):
    return {"User-Agent": NEW_DEVICE_UA, "Accept": "application/json",
            "Authorization": f"Bearer {access}"}


def leg_login(results, table, stack, timeout):
    label = "the discovered /auth opens a device-login challenge"
    route = table.get("auth")
    if route is None:
        check(results, label, False, "/auth is not announced")
        return None
    discovered = endpoint(route)
    paired = configured_base(stack, "auth")
    device = {"User-Agent": NEW_DEVICE_UA, "Accept": "application/json"}
    print(f"  dial  {discovered} (login: discovery only)", flush=True)

    status, _, text = http(discovered, "POST", "/auth/device-login", device,
                           None, timeout)
    check(results, label, status == 200,
          f"status={status} {body_text(text)[:200]!r}")
    if status != 200:
        return None
    challenge = info_of(text)
    challenge_id = challenge.get("challengeId", "")
    life = challenge.get("expiresAt", 0) - int(time.time())
    check(results, "the challenge is real material with a bounded life",
          len(challenge_id) == 64 and 0 < life <= 120,
          f"challengeId of {len(challenge_id)} char(s), expires in {life}s")

    poll = f"/auth/device-login/{challenge_id}"
    status, _, text = http(discovered, "GET", poll, device, None, timeout)
    check(results, "the new device's challenge waits for an approval",
          status == 200 and info_of(text).get("status") == "pending",
          f"status={status} {body_text(text)[:160]!r}")

    try:
        golden().run_seeder(stack, ["--roles"])
    except SystemExit as error:
        check(results, "the sandbox mints the pairing device a fresh session",
              False, str(error))
        return None
    sessions = json.loads((stack / "seed.json").read_text())["sessions"]
    try:
        access, _ = golden().exchange(paired, sessions["owner"], timeout)
    except SystemExit as error:
        check(results, "the paired device approves it over its own binding",
              False, str(error))
        return None
    status, _, text = http(paired, "POST", f"{poll}/approve",
                           {"User-Agent": golden().RECORDER_UA,
                            "Authorization": f"Bearer {access}"},
                           None, timeout)
    check(results, "the paired device approves it over its own binding",
          status == 200, f"status={status} {body_text(text)[:160]!r}")

    status, _, text = http(discovered, "GET", poll, device, None, timeout)
    granted = info_of(text)
    check(results, "the new device collects its own session",
          status == 200 and granted.get("status") == "approved"
          and bool(granted.get("accessToken"))
          and bool(granted.get("refreshToken"))
          and granted.get("userId") == GOLDEN_USER["id"]
          and granted.get("role") == GOLDEN_USER["role"],
          f"status={granted.get('status')!r} userId={granted.get('userId')} "
          f"role={granted.get('role')!r} name={granted.get('name')!r}")
    if not granted.get("accessToken"):
        return None

    status, _, text = http(discovered, "GET", poll, device, None, timeout)
    check(results, "the challenge is spent after its one collection",
          info_of(text).get("status") == "expired",
          f"status={status} {body_text(text)[:120]!r}")

    session = session_headers(granted["accessToken"])
    status, seconds, text = http(discovered, "GET", "/auth/status", session,
                                 None, timeout)
    check(results, "the session the client minted answers on the discovered "
          "endpoint", status == 200,
          f"status={status} in {seconds * 1000:.0f} ms "
          f"{body_text(text)[:160]!r}")

    status, _, text = http(discovered, "PATCH", "/auth/refresh-token",
                           {"User-Agent": NEW_DEVICE_UA},
                           json.dumps({"refreshToken":
                                       granted["refreshToken"]}), timeout)
    rotated = info_of(text) or {}
    check(results, "the session rotates from the discovered endpoint",
          status == 200 and bool(rotated.get("accessToken"))
          and bool(rotated.get("refreshToken"))
          and rotated.get("accessToken") != granted["accessToken"]
          and rotated.get("refreshToken") != granted["refreshToken"],
          f"status={status} {body_text(text)[:160]!r}")

    status, _, text = http(discovered, "PATCH", "/auth/refresh-token",
                           {"User-Agent": NEW_DEVICE_UA},
                           json.dumps({"refreshToken":
                                       granted["refreshToken"]}), timeout)
    check(results, "the rotated-away token is refused on the replay",
          status in (401, 403), f"status={status} {body_text(text)[:120]!r}")

    return session_headers(rotated.get("accessToken", ""))


def socket_for(results, label, base, headers, timeout, path):
    try:
        return isolation().SyncSocket(base, headers, timeout, path)
    except SystemExit as error:
        check(results, label, False, str(error))
        return None


def leg_bootstrap(results, table, session, timeout):
    label = "the discovered /sync greets the client the login minted"
    route = table.get("sync")
    if session is None or route is None:
        check(results, label, False, "no discovered session"
              if session is None else "/sync is not announced")
        return
    client = socket_for(results, label, endpoint(route), session, timeout,
                        route["request_path"])
    if client is None:
        return
    print(f"  dial  {endpoint(route)}{route['request_path']} (bootstrap)",
          flush=True)
    try:
        seen, greeting = client.collect(
            8.0, lambda opcode, payload: isolation().json_frame(opcode,
                                                                payload))
        info = (greeting or {}).get("info") or {}
        check(results, label,
              greeting is not None and greeting.get("operation") == 0
              and info.get("id") == GOLDEN_USER["id"]
              and info.get("role") == GOLDEN_USER["role"]
              and info.get("isActive") is True,
              f"{len(seen)} frame(s): "
              f"{json.dumps(greeting)[:200] if greeting else 'no greeting'}")

        started = time.monotonic()
        client.send_text(BOOTSTRAP_REQUEST)
        answers = {}
        refused = ""
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline and not answers and not refused:
            frame = client.receive(max(0.1, deadline - time.monotonic()))
            if frame is None:
                break
            parsed = isolation().json_frame(*frame)
            if parsed is None:
                continue
            if parsed.get("operation") != 1 or "status" in parsed:
                refused = isolation().frame_label(frame)
                continue
            answers = parsed.get("info") or {}
        waited = time.monotonic() - started
        absent = [name for name in BOOTSTRAP_TABLES if name not in answers]
        check(results, "every readable table answered the bootstrap",
              not absent and bool(answers),
              f"answered {sorted(answers)} of {sorted(BOOTSTRAP_TABLES)} "
              f"in {waited * 1000:.0f} ms"
              + (f", missing {absent}" if absent else "")
              + (f", refused {refused}" if refused else ""))

        users = (answers.get("user") or {}).get("created") or []
        cameras = (answers.get("camera") or {}).get("created") or []
        check(results, "the bootstrap carries the seeded rows",
              any(row.get("id") == GOLDEN_USER["id"]
                  and row.get("role") == GOLDEN_USER["role"]
                  for row in users)
              and any(row.get("id") == GOLDEN_CAMERA["id"]
                      and row.get("name") == GOLDEN_CAMERA["name"]
                      for row in cameras),
              f"{len(users)} user row(s), {len(cameras)} camera row(s)")
        check(results, "every table answer carries its rows and its watermark",
              bool(answers) and all(isinstance(entry, dict)
                                    and isinstance(entry.get("created"), list)
                                    and isinstance(entry.get("deleted"), list)
                                    and isinstance(entry.get("lastSyncDate"),
                                                   dict)
                                    for entry in answers.values()),
              ", ".join(sorted(answers)) or "no answer")

        client.send_text(WATERMARK_REQUEST)
        seen, contract = client.collect(
            6.0, lambda opcode, payload: isolation().audit_answer(opcode,
                                                                  payload))
        watermark = ((contract or {}).get("info") or {}).get("watermarkId")
        check(results, "the same socket still serves the sync contract",
              isinstance(watermark, int) and not isinstance(watermark, bool)
              and watermark >= 1,
              f"{len(seen)} frame(s): watermarkId={watermark}")
    finally:
        client.close()


def media_frame(opcode, payload, wanted):
    parsed = isolation().json_frame(opcode, payload)
    if parsed is None or parsed.get("type") != wanted:
        return None
    return parsed


def leg_media(results, table, session, timeout):
    label = "the discovered /media accepts a subscription"
    route = table.get("media")
    if session is None or route is None:
        check(results, label, False, "no discovered session"
              if session is None else "/media is not announced")
        return
    client = socket_for(results, label, endpoint(route), session, timeout,
                        route["request_path"])
    if client is None:
        return
    print(f"  dial  {endpoint(route)}{route['request_path']} (media)", flush=True)
    try:
        started = time.monotonic()
        client.send_text(MEDIA_SUBSCRIBE)
        seen, ready = client.collect(
            25.0, lambda opcode, payload: media_frame(opcode, payload,
                                                      "camera:ready"))
        payload = (ready or {}).get("payload") or {}
        check(results, label,
              ready is not None and payload.get("mime") == "video/mp4"
              and isinstance(payload.get("subId"), int)
              and not isinstance(payload.get("subId"), bool),
              f"{json.dumps(ready)[:200] if ready else 'no answer'} after "
              f"{time.monotonic() - started:.1f}s in {len(seen)} frame(s)")

        closed = None
        labels = []
        deadline = time.monotonic() + 25.0
        while time.monotonic() < deadline:
            frame = client.receive(deadline - time.monotonic())
            if frame is None:
                break
            labels.append(isolation().frame_label(frame))
            closed = media_frame(*frame, "camera:closed")
            if closed is not None:
                break
        reason = ((closed or {}).get("payload") or {}).get("reason")
        check(results, "the subscription reports why no frames can follow",
              reason == "upstream_failed",
              f"reason={reason!r} after {time.monotonic() - started:.1f}s"
              + (f" ({', '.join(labels)})" if labels else ""))
    finally:
        client.close()


def read_wav(path):
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit(f"{path} is not a RIFF/WAVE file")
    offset = 12
    fmt = None
    while offset + 8 <= len(data):
        chunk = data[offset:offset + 4]
        size = struct.unpack("<I", data[offset + 4:offset + 8])[0]
        body = data[offset + 8:offset + 8 + size]
        if chunk == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif chunk == b"data":
            channels, rate, bits = fmt[1], fmt[2], fmt[5]
            if (channels, rate, bits) != (1, 16000, 16):
                raise SystemExit(f"{path} is {channels}ch {rate}Hz {bits}bit; "
                                 "the voice wire is 1ch 16000Hz 16bit")
            return list(struct.unpack(f"<{len(body) // 2}h", body))
        offset += 8 + size + (size % 2)
    raise SystemExit(f"{path} carries no data chunk")


def pcm_chunks(samples):
    for start in range(0, len(samples), PCM_CHUNK_SAMPLES):
        block = samples[start:start + PCM_CHUNK_SAMPLES]
        yield struct.pack(f"<{len(block)}h", *block)


def llm_running():
    result = subprocess.run(["docker", "ps", "--format", "{{.Names}}"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        return False
    return any("argus-llm" in name for name in result.stdout.split())


def drain_voice(client, results, label, seconds, stop_on_assistant=True):
    spoken = []
    chunks = 0
    last_assistant = None
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        frame = client.receive(max(0.1, deadline - time.monotonic()))
        if frame is None:
            break
        opcode, payload = frame
        refusal = isolation().is_voice_refusal(opcode, payload)
        if refusal is not None:
            check(results, label, False,
                  "refused: " + json.dumps(refusal)[:200])
            return None
        if opcode == 0x2:
            chunks += 1
            continue
        parsed = isolation().json_frame(opcode, payload)
        if parsed is None:
            continue
        spoken.append(parsed)
        if parsed.get("type") == "voice:assistant":
            last_assistant = parsed
            if stop_on_assistant:
                break
    return {"spoken": spoken, "chunks": chunks, "assistant": last_assistant}


def leg_voice(results, table, session, timeout, audio):
    label = "the discovered /sync starts a voice turn"
    route = table.get("sync")
    if session is None or route is None:
        check(results, label, False, "no discovered session"
              if session is None else "/sync is not announced")
        return
    client = socket_for(results, label, endpoint(route), session, timeout,
                        route["request_path"])
    if client is None:
        return
    print(f"  dial  {endpoint(route)}{route['request_path']} (voice)", flush=True)
    try:
        client.collect(8.0, lambda opcode, payload:
                       isolation().json_frame(opcode, payload))
        started = time.monotonic()
        client.send_text(VOICE_START)
        greeting = drain_voice(client, results, label, GREETING_SECONDS)
        if greeting is None:
            return
        text = " ".join(str((frame.get("payload") or {}).get("text", ""))
                        for frame in greeting["spoken"]
                        if frame.get("type") == "voice:assistant")
        check(results, label, greeting["assistant"] is not None,
              f"{len(greeting['spoken'])} frame(s) in "
              f"{time.monotonic() - started:.1f}s")
        check(results, "the turn greets the user the client logged in as",
              "Golden" in text, f"{text.strip()[:200]!r}")
        check(results, "the greeting carries synthesized audio",
              greeting["chunks"] > 0,
              f"{greeting['chunks']} binary PCM frame(s)")

        if not audio:
            skip("the spoken turn: no argus-llm container is running")
        elif greeting["assistant"] is None:
            skip("the spoken turn: the greeting never arrived")
        else:
            time.sleep(GREETING_QUIET_SECONDS)
            samples = read_wav(WAV_FIXTURE)
            started = time.monotonic()
            sent = 0
            for chunk in pcm_chunks(samples):
                client.send_binary(chunk)
                sent += 1
            print(f"  sent  {len(samples)} samples of {WAV_FIXTURE.name} in "
                  f"{sent} binary frame(s)", flush=True)
            heard = None
            answer = []
            deadline = time.monotonic() + TURN_SECONDS
            while time.monotonic() < deadline:
                frame = client.receive(max(0.1, deadline - time.monotonic()))
                if frame is None:
                    break
                parsed = isolation().json_frame(*frame)
                if parsed is None:
                    continue
                if parsed.get("type") == "voice:stt":
                    heard = str((parsed.get("payload") or {}).get("text", ""))
                elif parsed.get("type") == "voice:assistant" and heard:
                    answer.append(parsed)
                    break
            check(results, "the audio the client sent was transcribed",
                  bool(heard and heard.strip()),
                  f"{heard!r} in {time.monotonic() - started:.1f}s")
            spoken = " ".join(str((frame.get("payload") or {}).get("text", ""))
                              for frame in answer)
            check(results, "the assistant answered the transcribed turn",
                  bool(answer), f"{spoken.strip()[:200]!r}")

        client.send_text(VOICE_STOP)
        done = None
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline and done is None:
            frame = client.receive(deadline - time.monotonic())
            if frame is None:
                break
            parsed = isolation().json_frame(*frame)
            if parsed is not None and parsed.get("type") == "voice:done":
                done = parsed
        check(results, "the turn closes when the client stops it",
              done is not None,
              json.dumps(done)[:160] if done else "no voice:done frame")
    finally:
        client.close()


def hold_after(results, stack, unit, profile, measured, label, rounds=6):
    status, seconds, text = 0, 0.0, ""
    for _ in range(rounds):
        time.sleep(0.5)
        status, seconds, text = http(configured_base(stack, unit), "GET",
                                     "/health", {}, None, 5.0)
        if status != 200:
            break
    pids = durability().service_pids(unit, profile)
    check(results, label, status == 200 and bool(pids) and pids == measured,
          f"pid {measured} -> {pids}, health status={status} in "
          f"{seconds * 1000:.0f} ms {body_text(text)[:80]!r}")


def command_state(stack, profile, table):
    print(f"routes announced under {SERVICE_TYPE}:")
    if not table:
        print("  none - is the sandbox up with ARGUS_STACK_MDNS=1?")
    for name, route in sorted(table.items()):
        tls = "https" if route["tls"] else "http"
        print(f"  {name:<18} {route['port']:<6} {tls:<6} "
              f"{', '.join(route['addresses']) or 'no address':<16} "
              f"{ROUTE_UNITS.get(name, 'foreign')}")
    print("sandbox units:")
    for unit in SANDBOX_UNITS:
        print(f"  {unit:<14} port {configured_port(stack, unit)} "
              f"pid {durability().service_pids(unit, profile)}")
    print("  argus-llm      " + ("running" if llm_running() else "not running"))


def main(argv):
    parser = argparse.ArgumentParser(usage=usage())
    parser.add_argument("--stack-dir", default=None,
                        help="sandbox directory (default build/native-stack)")
    parser.add_argument("--profile", default="dev", help="build profile")
    parser.add_argument("--timeout", type=float, default=25.0,
                        help="the socket timeout in seconds")
    parser.add_argument("drill", choices=("resolve", "login", "bootstrap",
                                          "media", "voice", "all", "state"))
    args = parser.parse_args(argv)
    stack = durability().stack_path(args.stack_dir)
    if not stack.is_dir():
        raise SystemExit(f"{stack} does not exist; run "
                         "scripts/native-stack.sh up first")
    if args.drill == "state":
        command_state(stack, args.profile, announced_routes(resolve(browse())))
        return 0
    durability().require_golden(stack)
    results = []
    before = {unit: durability().service_pids(unit, args.profile)
              for unit in ("auth", "sync", "camera")}
    table = discover(results, stack)
    command_state(stack, args.profile, table)
    session = None
    if args.drill in ("login", "bootstrap", "media", "voice", "all"):
        session = leg_login(results, table, stack, args.timeout)
    if args.drill in ("bootstrap", "all"):
        leg_bootstrap(results, table, session, args.timeout)
    if args.drill in ("media", "all"):
        leg_media(results, table, session, args.timeout)
    if args.drill in ("voice", "all"):
        leg_voice(results, table, session, args.timeout, llm_running())
    for unit, measured in before.items():
        hold_after(results, stack, unit, args.profile, measured,
                   f"{unit} is the same process after the client hung up")
    failed = [name for name, ok, _ in results if not ok]
    print(f"\ndiscovery: {len(results)} checks, "
          f"{len(results) - len(failed)} passed, {len(failed)} failed"
          + (f", {len(SKIPPED)} skipped" if SKIPPED else ""))
    for name in failed:
        print("  FAILED:", name)
    for reason in SKIPPED:
        print("  SKIPPED:", reason)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
