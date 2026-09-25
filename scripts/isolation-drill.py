#!/usr/bin/env python3
import argparse
import base64
import hashlib
import importlib.util
import json
import os
import re
import socket
import ssl
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
AI_SERVICES = ("llm", "stt", "tts", "vlm", "voice")
TRIO = ("auth", "sync", "camera")
READ_ROUTES = {"auth": "/auth/status", "camera": "/camera/1/status"}
READ_SERVED = {"auth": (200,), "camera": (200, 502)}
TALK_ROUTE = "/camera/1/talk"
TALK_BODY = json.dumps({"text": "prueba de aislamiento", "lang": "es"})
TTS_WORDS = "Text-to-speech unavailable"
VOICE_REFUSAL = "Voice unavailable"
VOICE_ERROR_TYPE = "voice:start_error"
VOICE_START = json.dumps({"type": "voice:start", "payload": {}})
WATERMARK_REQUEST = json.dumps({"type": "sync_audit_log",
                                "payload": {"findLast": True}})
VOICE_QUIET_SECONDS = 4.0
VOICE_BOUND_SECONDS = 5.0
LATENCY_BOUND_SECONDS = 5.0
CENSUS_TALLY = re.compile(r"verify: (\d+) probes checked, (\d+) failing, "
                          r"(\d+) stale, (\d+) probes unverified")
WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
STACK_UP_HINT = ("start the tier first:\n  docker compose -f "
                 "argus-deploy/docker-compose.yml up -d "
                 + " ".join("argus-" + name for name in AI_SERVICES))
DURABILITY = None


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def durability():
    global DURABILITY
    if DURABILITY is None:
        DURABILITY = load_module("durability_drill",
                                 REPO_ROOT / "scripts" / "durability-drill.py")
    return DURABILITY


def check(results, name, ok, detail=""):
    durability().check(results, name, ok, detail)


def stack_base(stack, unit):
    return durability().load_golden().stack_base(stack, unit)


def docker(arguments):
    result = subprocess.run(["docker", *arguments], capture_output=True,
                            text=True)
    if result.returncode != 0:
        raise SystemExit("docker " + " ".join(arguments) + " failed:\n"
                         + result.stdout + result.stderr)
    return result.stdout


def published_ports(field):
    ports = []
    for entry in field.split(","):
        match = re.search(r"([0-9a-fA-F.:\[\]]+):(\d+)(?:-(\d+))?->", entry)
        if match is None:
            continue
        host = match.group(1).strip("[]")
        if host in ("0.0.0.0", "::", ""):
            host = "127.0.0.1"
        first, last = int(match.group(2)), int(match.group(2))
        if match.group(3):
            last = int(match.group(3))
        for port in range(first, last + 1):
            ports.append({"host": host, "port": port})
    return ports


def ai_tier():
    rows = []
    pattern = "{{.ID}}\t{{.Names}}\t{{.Image}}\t{{.Ports}}"
    for line in docker(["ps", "--format", pattern]).splitlines():
        identifier, name, image, ports = (line.split("\t") + [""] * 4)[:4]
        service = image.split("/")[-1].split(":")[0].removeprefix("argus-")
        if service not in AI_SERVICES:
            continue
        rows.append({"id": identifier, "name": name, "service": service,
                     "image": image, "ports": published_ports(ports)})
    return sorted(rows, key=lambda row: row["service"])


def port_label(entry):
    return f"{entry['host']}:{entry['port']}"


def tier_reachable(tier):
    return all(not port_refuses(entry["port"], entry["host"])
               for row in tier for entry in row["ports"])


def require_tier_up(tier):
    if tier:
        return
    raise SystemExit("no argus AI container is running, so the drill has "
                     "nothing to take down and nothing to compare against; "
                     + STACK_UP_HINT)


def port_refuses(port, host="127.0.0.1", timeout=1.0):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return False
    except OSError:
        return True


def stop_tier(tier):
    for row in tier:
        row["stopped"] = True
    docker(["stop", *[row["id"] for row in tier]])
    return tier


def start_tier(tier):
    stopped = [row for row in tier if row.get("stopped")]
    if stopped:
        docker(["start", *[row["id"] for row in stopped]])


def tier_running(identifier):
    listed = docker(["ps", "--format", "{{.ID}}", "--filter",
                     f"id={identifier}"])
    return bool(listed.strip())


def host_port(base):
    host, _, port = base.split("://", 1)[-1].partition(":")
    return host, int(port)


class SyncSocket:
    def __init__(self, base, headers, timeout, path="/sync"):
        self.socket_timeout = timeout
        self.buffer = b""
        host, port = host_port(base)
        raw = socket.create_connection((host, port), timeout=timeout)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        self.socket = context.wrap_socket(raw, server_hostname=host)
        self._upgrade(host, port, headers, path)

    def _upgrade(self, host, port, headers, path):
        key = base64.b64encode(os.urandom(16)).decode()
        lines = [f"GET {path} HTTP/1.1", f"Host: {host}:{port}",
                 "Upgrade: websocket", "Connection: Upgrade",
                 f"Sec-WebSocket-Key: {key}", "Sec-WebSocket-Version: 13"]
        lines += [f"{name}: {value}" for name, value in headers.items()]
        self.socket.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise SystemExit("the sync socket closed during the upgrade")
            head += chunk
        head, _, self.buffer = head.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        if not re.match(r"HTTP/\d\.\d 101\b", lines[0]):
            raise SystemExit("the sync socket refused the upgrade: "
                             + lines[0])
        answer = None
        for line in lines[1:]:
            name, _, value = line.partition(":")
            if name.strip().lower() == "sec-websocket-accept":
                answer = value.strip()
        expected = base64.b64encode(hashlib.sha1(
            (key + WEBSOCKET_GUID).encode()).digest()).decode()
        if answer != expected:
            raise SystemExit("the sync socket's accept key does not match the "
                             f"key the client sent: {answer!r}")

    def _send(self, opcode, data):
        mask = os.urandom(4)
        header = bytearray([0x80 | opcode])
        size = len(data)
        if size < 126:
            header.append(0x80 | size)
        elif size < 65536:
            header.append(0x80 | 126)
            header += struct.pack("!H", size)
        else:
            header.append(0x80 | 127)
            header += struct.pack("!Q", size)
        masked = bytes(byte ^ mask[index % 4]
                       for index, byte in enumerate(data))
        self.socket.sendall(bytes(header) + mask + masked)

    def send_text(self, payload):
        self._send(0x1, payload.encode())

    def send_binary(self, payload):
        self._send(0x2, payload)

    def _take(self, count, deadline):
        while len(self.buffer) < count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.socket.settimeout(remaining)
            try:
                chunk = self.socket.recv(max(4096, count - len(self.buffer)))
            except (OSError, ssl.SSLError):
                return None
            if not chunk:
                return None
            self.buffer += chunk
        taken, self.buffer = self.buffer[:count], self.buffer[count:]
        return taken

    def receive(self, seconds):
        deadline = time.monotonic() + seconds
        message = b""
        while True:
            head = self._take(2, deadline)
            if head is None:
                return None
            final = bool(head[0] & 0x80)
            opcode = head[0] & 0x0F
            masked = bool(head[1] & 0x80)
            size = head[1] & 0x7F
            if size in (126, 127):
                width = 2 if size == 126 else 8
                extended = self._take(width, deadline)
                if extended is None:
                    return None
                size = struct.unpack("!H" if width == 2 else "!Q", extended)[0]
            mask = self._take(4, deadline) if masked else b"\0\0\0\0"
            payload = self._take(size, deadline) if size else b""
            if payload is None:
                return None
            if masked:
                payload = bytes(byte ^ mask[index % 4]
                                for index, byte in enumerate(payload))
            if opcode == 0x8:
                return None
            if opcode == 0x9:
                self._send(0xA, payload)
                continue
            if opcode == 0xA:
                continue
            message += payload
            if final:
                return opcode, message

    def collect(self, seconds, predicate):
        deadline = time.monotonic() + seconds
        seen = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return seen, None
            frame = self.receive(remaining)
            if frame is None:
                return seen, None
            seen.append(frame)
            match = predicate(*frame)
            if match is not None:
                return seen, match

    def ask(self, request, seconds, predicate):
        started = time.monotonic()
        self.send_text(request)
        seen, match = self.collect(seconds, predicate)
        return seen, match, time.monotonic() - started

    def close(self):
        try:
            self._send(0x8, b"")
            self.socket.close()
        except OSError:
            pass


def json_frame(opcode, payload):
    if opcode != 0x1:
        return None
    try:
        return json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        return None


def frame_label(frame):
    parsed = json_frame(*frame)
    if parsed is None:
        return f"opcode {frame[0]:#x} ({len(frame[1])} byte(s))"
    if "type" in parsed:
        return str(parsed["type"])
    if "operation" in parsed:
        return (f"operation={parsed['operation']} "
                f"option={parsed.get('option')!r}")
    return json.dumps(parsed)[:120]


def is_voice_refusal(opcode, payload):
    parsed = json_frame(opcode, payload)
    if parsed is None or parsed.get("type") != VOICE_ERROR_TYPE:
        return None
    return parsed


def is_audit_answer(opcode, payload):
    parsed = json_frame(opcode, payload)
    if parsed is None or parsed.get("operation") != 2:
        return None
    return parsed


def audit_answer(opcode, payload):
    parsed = is_audit_answer(opcode, payload)
    if parsed is None or "status" in parsed:
        return None
    watermark = (parsed.get("info") or {}).get("watermarkId")
    if not isinstance(watermark, int) or isinstance(watermark, bool) \
            or watermark < 1:
        return None
    return parsed


def audit_detail(envelope, waited):
    watermark = (envelope.get("info") or {}).get("watermarkId")
    return (f"operation={envelope.get('operation')} "
            f"option={envelope.get('option')!r} watermarkId={watermark} "
            f"in {waited * 1000:.0f} ms")


def status_label(status, text, seconds):
    if status == 0:
        return f"unreachable ({text.strip()[:160]}) in {seconds * 1000:.0f} ms"
    return f"status={status} in {seconds * 1000:.0f} ms"


def envelope_error(text, key):
    try:
        return json.loads(text)["errors"][key]
    except (ValueError, KeyError, TypeError):
        return ""


def timed_send(golden, base, method, path, headers, body, timeout=40.0):
    started = time.monotonic()
    try:
        status, _, text = golden.send(base, method, path, headers, body,
                                      "application/json", timeout)
    except SystemExit as error:
        return 0, time.monotonic() - started, str(error)
    return status, time.monotonic() - started, text


def owner_headers(stack, base_auth):
    golden = durability().load_golden()
    golden.run_seeder(stack, ["--token-only"])
    refresh = (stack / "refresh-token").read_text().strip()
    access, _ = golden.exchange(base_auth, refresh, 15.0)
    return {"User-Agent": golden.RECORDER_UA,
            "Authorization": f"Bearer {access}"}


def sync_socket(stack, headers, timeout, results, label):
    try:
        return SyncSocket(stack_base(stack, "sync"), headers, timeout)
    except SystemExit as error:
        check(results, label, False, str(error))
        return None


def hold_after(stack, results, profile, unit, measured, label, rounds=6):
    golden = durability().load_golden()
    base = stack_base(stack, unit)
    seconds, text = 0.0, ""
    for _ in range(rounds):
        time.sleep(0.5)
        status, seconds, text = timed_send(golden, base, "GET", "/health", {},
                                           None, 5.0)
        if status != 200:
            break
    pids = durability().service_pids(unit, profile)
    check(results, label,
          status == 200 and pids == measured["pids"][unit],
          f"pid {measured['pids'][unit]} -> {pids}, "
          + status_label(status, text, seconds))


def sync_read(stack, results, headers, timeout, label):
    connection = sync_socket(stack, headers, timeout, results, label)
    if connection is None:
        return
    try:
        seen, envelope, waited = connection.ask(
            WATERMARK_REQUEST, VOICE_QUIET_SECONDS + VOICE_BOUND_SECONDS,
            audit_answer)
        if envelope is None:
            check(results, label, False,
                  f"no audit answer in {len(seen)} frame(s): "
                  + ", ".join(frame_label(frame) for frame in seen[:6]))
            return
        check(results, label, True, audit_detail(envelope, waited))
    finally:
        connection.close()


def baseline(stack, monitor, profile, results, headers):
    measured = {"read": {}, "pids": {}}
    golden = durability().load_golden()
    for unit in TRIO:
        base = durability().ensure_up(stack, unit, profile, results)
        measured["pids"][unit] = durability().service_pids(unit, profile)
        status, seconds, text = timed_send(golden, base, "GET", "/health", {},
                                           None)
        check(results, f"{unit} answers /health before the tier is stopped",
              status == 200, status_label(status, text, seconds))
        route = READ_ROUTES.get(unit)
        if route is None:
            continue
        status, seconds, text = timed_send(golden, base, "GET", route, headers,
                                           None)
        measured["read"][unit] = status
        check(results, f"{unit}'s {route} answers a live caller before the "
              "tier is stopped", status in READ_SERVED[unit],
              status_label(status, text, seconds)
              + f" (expected {READ_SERVED[unit]})")
        print(f"    {unit} {route} -> {status} in {seconds * 1000:.0f} ms "
              "(the reading the outage has to leave alone)", flush=True)
    return measured


def control_talk(stack, results, headers):
    golden = durability().load_golden()
    status, seconds, text = timed_send(golden, stack_base(stack, "camera"),
                                       "POST", TALK_ROUTE, headers, TALK_BODY)
    message = envelope_error(text, "message")
    check(results, "with the tier up the talk path does not blame speech",
          status == 502 and TTS_WORDS not in message,
          f"status={status} code={envelope_error(text, 'code')} "
          f"message={message!r} in {seconds:.2f}s")
    return {"status": status, "code": envelope_error(text, "code"),
            "message": message, "seconds": seconds}


def control_voice(stack, results, headers, timeout):
    label = ("with the tier up the socket still serves the sync contract "
             "after voice:start")
    connection = sync_socket(stack, headers, timeout, results, label)
    if connection is None:
        return
    try:
        seen, refusal, waited = connection.ask(
            VOICE_START, VOICE_QUIET_SECONDS, is_voice_refusal)
        labels = ", ".join(frame_label(frame) for frame in seen[:6])
        detail = f"{len(seen)} frame(s) in {waited:.1f}s: {labels}"
        check(results, "with the tier up the voice leg does not refuse",
              refusal is None, detail)
        seen, envelope, waited = connection.ask(
            WATERMARK_REQUEST, VOICE_QUIET_SECONDS + VOICE_BOUND_SECONDS,
            audit_answer)
        if envelope is None:
            check(results, label, False,
                  f"no audit answer in {len(seen)} frame(s): "
                  + ", ".join(frame_label(frame) for frame in seen[:6]))
            return
        check(results, label, True, audit_detail(envelope, waited))
    finally:
        connection.close()


def drill_quiet(stack, monitor, profile, results, tier, measured, headers,
                timeout, control_counts):
    print("=== tier: the AI tier is down and auth, sync and camera keep "
          "serving ===", flush=True)
    for row in tier:
        check(results, f"the {row['service']} container is stopped",
              not tier_running(row["id"]),
              f"container {row['id']} {row['name']}")
        check(results, f"the {row['service']} container publishes a port the "
              "drill can probe", bool(row["ports"]),
              ", ".join(port_label(entry) for entry in row["ports"]))
        for entry in row["ports"]:
            refused = durability().wait_for(
                lambda: port_refuses(entry["port"], entry["host"]), 10)
            check(results, f"nothing answers the {row['service']} port the "
                  "container published", bool(refused), port_label(entry))

    golden = durability().load_golden()
    for unit in TRIO:
        base = stack_base(stack, unit)
        status, seconds, text = timed_send(golden, base, "GET", "/health", {},
                                           None)
        check(results, f"{unit} answers /health with the AI tier down",
              status == 200 and seconds < LATENCY_BOUND_SECONDS,
              status_label(status, text, seconds))
        route = READ_ROUTES.get(unit)
        if route is None:
            continue
        status, seconds, text = timed_send(golden, base, "GET", route, headers,
                                           None)
        check(results, f"{unit}'s {route} answers as it did before the outage",
              status == measured["read"][unit],
              f"{measured['read'][unit]} -> "
              + status_label(status, text, seconds))
        check(results, f"{unit}'s {route} answers within the bound",
              seconds < LATENCY_BOUND_SECONDS, f"{seconds * 1000:.0f} ms")
    sync_read(stack, results, headers, timeout,
              "sync still answers its contract with the AI tier down")

    status, seconds, text = timed_send(golden, stack_base(stack, "sync"),
                                       "GET", "/health", {}, None)
    pids = durability().service_pids("sync", profile)
    check(results, "sync is the same process after the contract probe",
          status == 200 and bool(pids) and pids == measured["pids"]["sync"],
          f"pid {measured['pids']['sync']} -> {pids}, "
          + status_label(status, text, seconds))

    replay = run_replay(stack, results, profile)
    check(results, "the frozen /sync replay still passes with the tier down",
          replay_passed(replay), replay_detail(replay))

    census = run_census(stack)
    counts = census_counts(census.stdout + census.stderr)
    detail = census_tally(census.stdout + census.stderr)
    if control_counts is None:
        detail += " (the census was already red before the outage)"
    check(results, "the HTTP contract census still passes with the tier down",
          counts is not None and counts == control_counts, detail)

    audit_write(stack, results)


def audit_write(stack, results):
    drill = durability()
    before = drill.outbox_rows(stack)
    baseline_id = before[-1]["id"] if before else 0
    row = drill.logout_action(stack, stack_base(stack, "auth"), baseline_id)
    check(results, "an audited write still commits with the AI tier down",
          drill.first_try_ack(row), drill.describe(row))
    drill.assert_one_row(stack, results, row,
                         "the action committed with the AI tier down")


def drill_talk(stack, results, control, headers):
    print("=== talk: the camera's speech leg refuses in the camera's own "
          "words ===", flush=True)
    golden = durability().load_golden()
    status, seconds, text = timed_send(golden, stack_base(stack, "camera"),
                                       "POST", TALK_ROUTE, headers, TALK_BODY)
    code = envelope_error(text, "code")
    message = envelope_error(text, "message")
    check(results, "the talk path answers on the camera's own route",
          status == control["status"] and code == control["code"],
          f"{status_label(status, text, seconds)} code={code}")
    check(results, "the refused talk names the speech leg that went away",
          TTS_WORDS in message and "argus-tts" in message,
          f"message={message!r}")
    check(results, "the answer changed because the tier went, not the route",
          message != control["message"],
          f"{control['message']!r} -> {message!r}")
    check(results, "the camera refuses the talk path within the bound",
          seconds < LATENCY_BOUND_SECONDS,
          f"{seconds * 1000:.0f} ms, against {control['seconds']:.2f}s on the "
          "device path alone")


def drill_voice(stack, results, headers, timeout):
    print("=== voice: sync's voice leg refuses per frame and serves on ===",
          flush=True)
    connection = sync_socket(stack, headers, timeout, results,
                             "the socket opens with the AI tier down")
    if connection is None:
        return
    try:
        seen, refusal, waited = connection.ask(
            VOICE_START, VOICE_QUIET_SECONDS + VOICE_BOUND_SECONDS,
            is_voice_refusal)
        check(results, "the voice leg refuses voice:start with the tier down",
              refusal is not None,
              f"{len(seen)} frame(s) in {waited * 1000:.0f} ms: "
              + ", ".join(frame_label(frame) for frame in seen[:6]))
        if refusal is None:
            return
        check(results, "the refusal is the leg's own answer",
              refusal.get("status") == 503
              and VOICE_REFUSAL in str(refusal.get("error")),
              f"status={refusal.get('status')} "
              f"error={refusal.get('error')!r}")
        check(results, "the refusal comes from the leg's own probe window, "
              "not a client timeout",
              waited < VOICE_BOUND_SECONDS, f"{waited * 1000:.0f} ms")

        seen, second, waited = connection.ask(VOICE_START,
                                              VOICE_QUIET_SECONDS,
                                              is_voice_refusal)
        check(results, "the same socket refuses the second voice:start too",
              second is not None and second.get("status") == 503,
              f"{len(seen)} frame(s) in {waited * 1000:.0f} ms")
        if second is None:
            return

        seen, envelope, waited = connection.ask(
            WATERMARK_REQUEST, VOICE_QUIET_SECONDS + VOICE_BOUND_SECONDS,
            audit_answer)
        if envelope is None:
            check(results, "the same socket still answers the sync contract",
                  False,
                  f"no audit answer in {len(seen)} frame(s): "
                  + ", ".join(frame_label(frame) for frame in seen[:6]))
            return
        check(results, "the same socket still answers the sync contract",
              True, audit_detail(envelope, waited) + ", after the refusal")
    finally:
        connection.close()


def run_census(stack, timeout=900.0):
    return subprocess.run(
        [sys.executable, str(REPO_ROOT / "scripts" / "golden-http.py"),
         "verify", "--stack-dir", str(stack)],
        capture_output=True, text=True, timeout=timeout)


def census_counts(output):
    match = CENSUS_TALLY.search(output)
    if match is None:
        return None
    return tuple(int(value) for value in match.groups())


def census_tally(output):
    counts = census_counts(output)
    if counts is None:
        return "the census printed no tally: " + output.strip()[-300:]
    return (f"{counts[0]} probes checked, {counts[1]} failing, "
            f"{counts[2]} stale, {counts[3]} probes unverified")


def census_control(stack, results):
    census = run_census(stack)
    counts = census_counts(census.stdout + census.stderr)
    check(results, "the HTTP contract census is green before the tier is "
          "stopped",
          census.returncode == 0 and counts is not None
          and counts[1:] == (0, 0, 0),
          census_tally(census.stdout + census.stderr))
    return counts


def stack_env(stack, profile):
    result = subprocess.run(
        [str(REPO_ROOT / "scripts" / "native-stack.sh"), "env"],
        capture_output=True, text=True,
        env={**os.environ, "ARGUS_STACK_DIR": str(stack),
             "ARGUS_STACK_PROFILE": profile})
    if result.returncode != 0:
        raise SystemExit("native-stack.sh env failed:\n" + result.stdout
                         + result.stderr)
    environment = dict(os.environ)
    for line in result.stdout.splitlines():
        if line.startswith("export "):
            name, _, value = line[len("export "):].partition("=")
            environment[name] = value
    return environment


def run_replay(stack, results, profile, timeout=600.0):
    binary = REPO_ROOT / "services" / "sync" / "build" / profile / "tests" / \
        "golden-sync-test"
    if not binary.is_file():
        check(results, "the frozen /sync replay is built", False,
              f"{binary} does not exist; build it with ./scripts/build-all.sh "
              f"{profile} --only sync")
        return None
    return subprocess.run([str(binary), "verify"], capture_output=True,
                          text=True, cwd=str(REPO_ROOT), timeout=timeout,
                          env=stack_env(stack, profile))


def replay_detail(replay):
    if replay is None:
        return "the replay did not run"
    output = replay.stdout + replay.stderr
    for line in output.splitlines():
        if line.startswith("PASS:") or line.startswith("FAIL"):
            return line.strip()
    lines = output.strip().splitlines()
    return lines[-1][:300] if lines else "the replay printed nothing"


def replay_passed(replay):
    if replay is None or replay.returncode != 0:
        return False
    return "PASS:" in (replay.stdout + replay.stderr)


def run_block(stack, monitor, profile, results, wanted, timeout):
    tier = ai_tier()
    require_tier_up(tier)
    print("the AI tier is up: "
          + ", ".join(f"{row['service']} {row['id']} on "
                      + ",".join(port_label(entry) for entry in row["ports"])
                      for row in tier), flush=True)
    check(results, "every AI container publishes a port the drill can probe",
          all(row["ports"] for row in tier),
          ", ".join(f"{row['service']} {len(row['ports'])} port(s)"
                    for row in tier))
    check(results, "the AI tier answers on every port it publishes before "
          "the outage", tier_reachable(tier),
          ", ".join(f"{row['service']} "
                    + ",".join(port_label(entry) for entry in row["ports"])
                    for row in tier))
    control_counts = census_control(stack, results) if "tier" in wanted \
        else None
    auth_base = stack_base(stack, "auth")
    headers = owner_headers(stack, auth_base)
    measured = baseline(stack, monitor, profile, results, headers)
    controls = {}
    if "talk" in wanted:
        controls["talk"] = control_talk(stack, results, headers)
    if "voice" in wanted:
        control_voice(stack, results, headers, timeout)
        hold_after(stack, results, profile, "sync", measured,
                   "sync is the same process after the control socket hung up")

    try:
        stop_tier(tier)
        if "tier" in wanted:
            drill_quiet(stack, monitor, profile, results, tier, measured,
                        headers, timeout, control_counts)
            headers = owner_headers(stack, auth_base)
        if "talk" in wanted:
            drill_talk(stack, results, controls["talk"], headers)
            hold_after(stack, results, profile, "camera", measured,
                       "the camera is the same process after the refusal")
        if "voice" in wanted:
            drill_voice(stack, results, headers, timeout)
            hold_after(stack, results, profile, "sync", measured,
                       "sync is the same process after the hang-up")
    finally:
        start_tier(tier)
        back = durability().wait_for(lambda: tier_reachable(tier), 120)
        check(results, "the AI tier is back", bool(back),
              ", ".join(row["service"] for row in tier))


def command_state(stack, profile):
    print("AI tier:")
    for row in ai_tier():
        ports = ",".join(port_label(entry) for entry in row["ports"])
        print(f"  {row['service']:<6} {row['id']} {row['name']} ports={ports}")
    golden = durability().load_golden()
    for unit in TRIO:
        base = stack_base(stack, unit)
        if not base:
            print(f"{unit}: no sandbox config")
            continue
        status, seconds, _ = timed_send(golden, base, "GET", "/health", {},
                                        None, 5.0)
        print(f"{unit}: {base} health={status} in {seconds * 1000:.0f} ms "
              f"pid={durability().service_pids(unit, profile)}")


def main(argv):
    parser = argparse.ArgumentParser(
        description="Drill the isolation of the AI tier: stop every argus AI "
                    "container and measure that auth, sync and camera keep "
                    "serving, that the camera's speech leg and sync's voice "
                    "leg refuse in their own words, and that the sockets stay "
                    "open.")
    parser.add_argument("--stack-dir", default=None,
                        help="sandbox directory (default build/native-stack)")
    parser.add_argument("--monitor", default="http://127.0.0.1:8222",
                        help="the NATS monitoring endpoint")
    parser.add_argument("--profile", default="dev", help="build profile")
    parser.add_argument("--timeout", type=float, default=20.0,
                        help="the socket timeout in seconds")
    parser.add_argument("drill", choices=("tier", "talk", "voice", "all",
                                          "state"))
    args = parser.parse_args(argv)
    stack = durability().stack_path(args.stack_dir)
    if not stack.is_dir():
        raise SystemExit(f"{stack} does not exist; run "
                         "scripts/native-stack.sh up first")
    if args.drill == "state":
        command_state(stack, args.profile)
        return 0
    durability().require_golden(stack)
    results = []
    wanted = ("tier", "talk", "voice")
    if args.drill != "all":
        wanted = (args.drill,)
    run_block(stack, args.monitor, args.profile, results, wanted, args.timeout)
    failed = [name for name, ok, _ in results if not ok]
    print(f"\nisolation: {len(results)} checks, "
          f"{len(results) - len(failed)} passed, {len(failed)} failed")
    for name in failed:
        print("  FAILED:", name)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
