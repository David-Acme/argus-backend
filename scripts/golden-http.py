#!/usr/bin/env python3
import argparse
import base64
import importlib.util
import json
import os
import re
import secrets
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import time
import tomllib
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
DEVICE_CREDENTIAL_HEADER = "X-Argus-Device-Credential"
CREDENTIALS = {}

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

ABSENT_DEPENDENCY_ANSWERS = {
    ("sync", "/rtc/token"): (503, "RTC_UNAVAILABLE"),
}

PUSH_TOKEN = "argus-golden-push-token"
UNREGISTERED_TOKEN = "argus-golden-unregistered-token"
RING_OPERATION = 8
PRIMED_ROUTES = {("sync", "/sync/heartbeat")}
PLACEHOLDER = "<BYTES>"
GOLDEN_JPEG_B64 = "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAYEBQYFBAYGBQYHBwYIChAKCgkJChQODwwQFxQYGBcUFhYaHSUfGhsjHBYWICwgIyYnKSopGR8tMC0oMCUoKSj/2wBDAQcHBwoIChMKChMoGhYaKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCj/wAARCABAAEADASIAAhEBAxEB/8QAFwABAQEBAAAAAAAAAAAAAAAAAAMECP/EABUQAQEAAAAAAAAAAAAAAAAAAAAS/8QAFwEBAQEBAAAAAAAAAAAAAAAAAAYDB//EABYRAQEBAAAAAAAAAAAAAAAAAAAUYf/aAAwDAQACEQMRAD8A5/gheCHertZxoQQvBBdpGhBC8EF2kaEELwQXaRtMELwQjrtVcaEELwQXaRoQQvBBdpGhBC8EF2kbTBC8EI27VZGhBC8EF2kaEELwQXaRoQQvBBdpG0QQ0wQjrtVcbNBDTBBdpGzQQ0wQXaRs0ENMEF2kb//Z"
GENERATED = "<GENERATED>"
INITIAL_INFO_OPERATION = 0
STACK_DIR = None
OWNER_ID = None

CONTENT_TYPE_OVERRIDES = {
    ("stt", "POST", "/stt/v1/transcribe"): "audio/x-argus-pcm-s16",
}

BODY_OVERRIDES = {
    ("auth", "PATCH", "/auth/me"): json.dumps({"name": "p" * 125}),
    ("identity", "POST", "/invitation"): json.dumps({"role": "owner"}),
    ("auth", "POST", "/auth/device-login"): json.dumps(
        {"pollHash": "0" * 64}),
    ("camera", "POST", "/camera/probe"): json.dumps(
        {"driver": "tapo", "ip": "10.255.255.1", "port": 1}),
    ("guard", "POST", "/guard/mode"): json.dumps(
        {"mode": "argus-probe", "environmentId": 0}),
    ("settings", "PATCH", "/settings/{1}"): json.dumps(
        {"changes": [{"key": "probe.setting", "value": "argus-probe"}]}),
    ("notification", "POST", "/notification-token"): json.dumps(
        {"token": PUSH_TOKEN, "platform": "android", "lang": "es"}),
    ("notification", "DELETE", "/notification-token"): json.dumps(
        {"token": UNREGISTERED_TOKEN}),
    ("llm", "POST", "/llm/v1/chat"): json.dumps(
        {"messages": [{"role": "user", "content": "Responde con una sola palabra: hola."}],
         "max_tokens": 8, "temperature": 0.0, "reset_context": True, "tools": False}),
    ("llm", "POST", "/llm/v1/chat-stream"): json.dumps(
        {"messages": [{"role": "user", "content": "Responde con una sola palabra: hola."}],
         "max_tokens": 8, "temperature": 0.0, "reset_context": True, "tools": False}),
    ("llm", "POST", "/llm/v1/screen"): json.dumps(
        {"text": "Hola, ¿qué hay de nuevo?"}),
    ("tts", "POST", "/tts/v1/synthesize"): json.dumps(
        {"text": "Hola.", "style_id": "M3", "lang": "es"}),
    ("tts", "POST", "/tts/v1/synthesize-stream"): json.dumps(
        {"text": "Hola.", "style_id": "M3", "lang": "es"}),
    ("vlm", "POST", "/vlm/v1/describe"): json.dumps(
        {"image_b64": GOLDEN_JPEG_B64, "prompt": "Describe la imagen en una palabra."}),
    ("stt", "POST", "/stt/v1/transcribe"): "\x00" * 32000,
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

EXTRA_PROBES = {
    ("notification", "DELETE", "/notification-token"): (
        {"name": "owner-registered-token", "auth": "owner",
         "path": "/notification-token",
         "before": ({"method": "POST", "path": "/notification-token",
                     "body": json.dumps({"token": UNREGISTERED_TOKEN,
                                         "platform": "android",
                                         "lang": "es"})},)},
    ),
    ("sync", "POST", "/rtc/decline"): (
        {"name": "owner-ringing-call", "auth": "owner",
         "path": "/rtc/decline",
         "before": ({"ring": {
             "caller": "guard", "target": "notification",
             "method": "argus.notification.v1.NotificationService/CreateNotifications",
             "request": {"type": "camera", "title": "Golden panic",
                         "body": "Fixture row"},
             "data": {"kind": "guard_panic", "urgency": "critical",
                      "lang": "es"},
             "remember": "ringingCall"}},),
         "body": json.dumps({"callId": "{id:ringingCall}"})},
    ),
    ("auth", "DELETE", "/auth/sessions"): (
        {"name": "resident-scope-others", "auth": "resident",
         "path": "/auth/sessions?scope=others"},
        {"name": "guest-scope-all", "auth": "guest",
         "path": "/auth/sessions?scope=all"},
    ),
    ("auth", "DELETE", "/auth/sessions/{1}"): (
        {"name": "guest-own-session", "auth": "guest",
         "path": "/auth/sessions/{session:guest}"},
    ),
    ("auth", "DELETE", "/auth/users/{1}/sessions/{2}"): (
        {"name": "owner-resident-session", "auth": "owner",
         "path": "/auth/users/{user:resident}/sessions/{session:resident}"},
    ),
    ("identity", "PATCH", "/user/{1}"): (
        {"name": "owner-resident-role", "auth": "owner",
         "path": "/user/{user:resident}",
         "body": json.dumps({"role": "resident"})},
        {"name": "owner-guard-role-while-surveillance-off", "auth": "owner",
         "path": "/user/{user:resident}",
         "body": json.dumps({"role": "guard"}),
         "modulesOff": ("surveillance",),
         "after": {"method": "PATCH", "body": json.dumps({"role": "resident"})}},
    ),
    ("identity", "POST", "/invitation"): (
        {"name": "owner-guard-while-surveillance-off", "auth": "owner",
         "path": "/invitation",
         "body": json.dumps({"role": "guard"}),
         "modulesOff": ("surveillance",)},
    ),
    ("identity", "GET", "/invitation"): (
        {"name": "owner-module-revoked", "auth": "owner",
         "path": "/invitation",
         "before": ({"method": "POST", "path": "/invitation",
                     "body": json.dumps({"role": "guard"})},),
         "modulesOff": ("surveillance",),
         "settle": True},
    ),
    ("settings", "GET", "/modules/{1}/impact"): (
        {"name": "owner-surveillance-disable", "auth": "owner",
         "path": "/modules/surveillance/impact?action=disable"},
        {"name": "owner-surveillance-uninstall", "auth": "owner",
         "path": "/modules/surveillance/impact?action=uninstall"},
        {"name": "owner-productivity-disable", "auth": "owner",
         "path": "/modules/productivity/impact?action=disable"},
        {"name": "owner-core-refused", "auth": "owner",
         "path": "/modules/core/impact?action=disable"},
        {"name": "owner-surveillance-uninstall-with-holder", "auth": "owner",
         "path": "/modules/surveillance/impact?action=uninstall",
         "before": ({"unit": "identity", "method": "PATCH",
                     "path": "/user/{user:resident}",
                     "body": json.dumps({"role": "guard"})},
                    {"unit": "identity", "method": "POST",
                     "path": "/invitation",
                     "body": json.dumps({"role": "guard"}),
                     "remember": ("pendingInvitation", "info.id")}),
         "after": ({"unit": "identity", "method": "PATCH",
                    "path": "/user/{user:resident}",
                    "body": json.dumps({"role": "resident"}),
                    "tolerate": True},
                   {"unit": "identity", "method": "DELETE",
                    "path": "/invitation/{id:pendingInvitation}",
                    "tolerate": True})},
    ),
    ("settings", "POST", "/modules/{1}/request"): (
        {"name": "resident-productivity-off", "auth": "resident",
         "path": "/modules/productivity/request",
         "modulesOff": ("productivity",)},
        {"name": "guest-productivity-off", "auth": "guest",
         "path": "/modules/productivity/request",
         "modulesOff": ("productivity",)},
        {"name": "resident-productivity-off-again", "auth": "resident",
         "path": "/modules/productivity/request",
         "modulesOff": ("productivity",)},
        {"name": "owner-productivity-off", "auth": "owner",
         "path": "/modules/productivity/request",
         "modulesOff": ("productivity",)},
        {"name": "resident-module-on", "auth": "resident",
         "path": "/modules/productivity/request"},
        {"name": "resident-coming-soon", "auth": "resident",
         "path": "/modules/agronomy/request"},
    ),
    ("settings", "POST", "/modules/{1}/uninstall"): (
        {"name": "owner-surveillance-roles-held", "auth": "owner",
         "path": "/modules/surveillance/uninstall",
         "body": json.dumps({"keepData": True}),
         "before": ({"unit": "identity", "method": "PATCH",
                     "path": "/user/{user:resident}",
                     "body": json.dumps({"role": "guard"})},),
         "after": ({"unit": "identity", "method": "PATCH",
                    "path": "/user/{user:resident}",
                    "body": json.dumps({"role": "resident"}),
                    "tolerate": True},)},
        {"name": "owner-surveillance-reassign", "auth": "owner",
         "path": "/modules/surveillance/uninstall",
         "body": json.dumps({"keepData": True,
                             "reassign": {"{user:resident}": "resident"}}),
         "before": ({"unit": "identity", "method": "PATCH",
                     "path": "/user/{user:resident}",
                     "body": json.dumps({"role": "guard"})},),
         "modulesOff": ("surveillance",)},
    ),
    ("productivity", "POST", "/reminder"): (
        {"name": "owner-create", "auth": "owner", "path": "/reminder",
         "body": json.dumps({"title": "Golden created",
                             "scheduledAt": 4102444800}),
         "remember": ("createdReminder", "info.id"),
         "after": ({"method": "DELETE", "path": "/reminder/{id:createdReminder}",
                    "tolerate": True},)},
        {"name": "resident-create-ignores-target", "auth": "resident",
         "path": "/reminder",
         "body": json.dumps({"title": "Golden resident",
                             "scheduledAt": 4102444800,
                             "targetUserId": 1}),
         "remember": ("residentReminder", "info.id"),
         "after": ({"method": "DELETE", "auth": "resident",
                    "path": "/reminder/{id:residentReminder}",
                    "tolerate": True},)},
    ),
    ("productivity", "PATCH", "/reminder/{1}"): (
        {"name": "owner-complete-own", "auth": "owner",
         "path": "/reminder/{id:patchReminder}",
         "body": json.dumps({"isCompleted": True}),
         "before": ({"method": "POST", "path": "/reminder",
                     "body": json.dumps({"title": "Golden patch target",
                                         "scheduledAt": 4102444800}),
                     "remember": ("patchReminder", "info.id")},),
         "after": ({"method": "DELETE", "path": "/reminder/{id:patchReminder}",
                    "tolerate": True},)},
        {"name": "resident-foreign-row", "auth": "resident",
         "path": "/reminder/{id:foreignPatch}",
         "body": json.dumps({"title": "not yours"}),
         "before": ({"method": "POST", "path": "/reminder",
                     "body": json.dumps({"title": "Golden owner row",
                                         "scheduledAt": 4102444800}),
                     "remember": ("foreignPatch", "info.id")},),
         "after": ({"method": "DELETE", "path": "/reminder/{id:foreignPatch}",
                    "tolerate": True},)},
    ),
    ("productivity", "DELETE", "/reminder/{1}"): (
        {"name": "owner-delete-own", "auth": "owner",
         "path": "/reminder/{id:deleteReminder}",
         "before": ({"method": "POST", "path": "/reminder",
                     "body": json.dumps({"title": "Golden delete target",
                                         "scheduledAt": 4102444800}),
                     "remember": ("deleteReminder", "info.id")},)},
        {"name": "resident-foreign-row", "auth": "resident",
         "path": "/reminder/{id:foreignDelete}",
         "before": ({"method": "POST", "path": "/reminder",
                     "body": json.dumps({"title": "Golden owner row",
                                         "scheduledAt": 4102444800}),
                     "remember": ("foreignDelete", "info.id")},),
         "after": ({"method": "DELETE", "path": "/reminder/{id:foreignDelete}",
                    "tolerate": True},)},
    ),
}

MODULE_GATES = {
    ("identity", "surveillance"): "/visitor-settings",
}

MODULE_WAIT_SECONDS = 90.0
RING_WINDOW_SECONDS = 150.0
RING_ATTEMPT_SECONDS = 12.0
RING_RETRY_SECONDS = 4.0
MODULE_POLL_SECONDS = 0.25

CHALLENGE_DETAILS = "/auth/device-login/{1}/details"
CHALLENGE_SLOT = "{challenge}"
MODULE_DATA = "/modules/{1}/data"
MODULE_DATA_ID = "productivity"
SESSION_SLOT_RE = re.compile(r"\{session:([a-z]+)\}")
RUNTIME_SLOT_RE = re.compile(r"\{id:([A-Za-z]+)\}")
USER_SLOT_RE = re.compile(r"\{user:([a-z]+)\}")
RUNTIME_IDS = {}
SETTLE_SECONDS = 1.0

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
    ("auth", "DELETE", "/auth/sessions/{1}"),
    ("auth", "DELETE", "/auth/users/{1}/sessions/{2}"),
}

VOLATILE_FIELDS = {
    ("sync", "/sync/heartbeat"): {
        "keys": ("at", "presenceSince"),
        "reason": "a heartbeat stamps the second it answers and the presence "
                  "change it reports, so both move on every call; the guard "
                  "state, the interval, grace and armed state stay pinned, and "
                  "the guard state is seeded by seed-golden.py, which publishes "
                  "one guard heartbeat before the recorder's probes",
    },
    ("settings", "/modules/{1}/data"): {
        "keys": (),
        "placeholders": ("owners[].bytes",),
        "reason": "bytes is the disk size of an owner's data on the answering "
                  "host, so it is read as the placeholder <BYTES> and the "
                  "answer's shape is still compared; the counts stay pinned",
    },
    ("sync", "/sync/activity"): {
        "keys": ("items", "nextCursor"),
        "reason": "the Owner's activity history grows with every write the "
                  "fleet makes, and the rows reach argus-sync asynchronously "
                  "over NATS, so the page a probe reads depends on how many "
                  "had landed; the envelope, the access answers beside it and "
                  "the filters and paging (pinned by sync-activity-test) stay "
                  "checked",
    },
    ("settings", "/modules/{1}/impact"): {
        "keys": ("data", "filesBytes"),
        "reason": "the module's data summary is read live from five owners "
                  "(the synced change history arrives asynchronously over "
                  "NATS) and the bytes of its files come from the model files "
                  "present on the answering host; the refusal, the stops, the "
                  "role holders, the invitations, the roles an uninstall may "
                  "give and what keeps running stay pinned, and the data "
                  "summary itself is pinned by GET /modules/{id}/data",
    },
    ("notification", "/notification/delivery-summary"): {
        "keys": ("latencyMsMax", "latencyMsP50", "latencyMsP95",
                 "probeMs", "probeOk", "sent", "unacked"),
        "reason": "the push channel's self-test runs on its own schedule, so "
                  "these fields report the last probe's outcome rather than a "
                  "contract; sent and unacked count deliveries that drain "
                  "asynchronously over NATS, so a probe can land before or "
                  "after the seeded rows' last delivery (seen 7 or 9 on fresh "
                  "sandboxes); the other counters stay pinned",
    },
    ("camera", "/camera/overview"): {
        "keys": ("cameras[].health",),
        "reason": "a camera's health is the stream supervisor's last "
                  "observation, unknown until its first probe and unreachable "
                  "after it for the fixture camera; the rest of each row stays "
                  "pinned",
    },
    ("settings", "/modules"): {
        "keys": ("modules[].hardware", "modules[].installedBytes",
                 "modules[].components"),
        "reason": "the hardware verdict, free disk and installed bytes are "
                  "read from the host that answers, and a component's state "
                  "comes from the model files and engines present on it; the "
                  "catalog, lifecycle, sizes and jobs beside them stay pinned",
    },
    ("settings", "/settings/profiles"): {
        "keys": ("recommendation",),
        "reason": "the recommendation is derived from the cores, RAM, ISA "
                  "and GPU of the host that answers, so it differs between "
                  "machines; the profile previews beside it stay pinned",
    },
    ("shared", "/health"): {
        "keys": ("internalTls",),
        "reason": "internalTls describes the internal certificate the "
                  "answering process loaded: its CA's subject is the "
                  "installation's own, the days left move every day and a CA "
                  "rotation puts two CAs in the bundle, so it is never "
                  "pinned here; lib/http's tls-health-test pins its content, "
                  "and every other /health field stays pinned",
    },
    ("guard", "/health"): {
        "keys": ("internalTls",),
        "reason": "guard's own /health has no envelope and carries "
                  "internalTls at its top level, beside status and service; "
                  "it describes the internal certificate the answering "
                  "process loaded, whose CA's subject is the installation's "
                  "own and whose days left move every day, so it is never "
                  "pinned here; lib/http's tls-health-test pins its content, "
                  "and status and service stay pinned",
    },
    ("llm", "/llm/v1/chat"): {
        "keys": (),
        "generated": ("text",),
        "reason": "the model's reply is generated text, so its value is "
                  "<GENERATED>; the envelope, the key and the string type stay "
                  "compared, and an error answer is compared in full",
    },
    ("llm", "/llm/v1/screen"): {
        "keys": (),
        "generated": ("text",),
        "reason": "the screen answer is generated text",
    },
    ("llm", "/llm/v1/chat-stream"): {
        "keys": (),
        "generatedText": True,
        "reason": "the streamed reply is generated text on a 2xx answer, so the "
                  "body is <GENERATED>; the content type and status stay compared",
    },
    ("llm", "/llm/v1/config"): {
        "keys": (),
        "generated": ("lastDecodedTokens", "lastPromptTokens", "lastReusedTokens"),
        "reason": "the last-turn token counters are the model's own bookkeeping "
                  "of the reply it generated, so they are <GENERATED>; the loaded "
                  "model, its context and its defaults stay compared",
    },
    ("tts", "/tts/v1/synthesize"): {
        "keys": (),
        "generatedText": True,
        "reason": "the synthesized audio is model output, so the 2xx body is "
                  "<GENERATED>; the audio's content type stays compared",
    },
    ("tts", "/tts/v1/synthesize-stream"): {
        "keys": (),
        "generatedText": True,
        "reason": "the streamed audio is model output, so the 2xx body is "
                  "<GENERATED>; the audio's content type stays compared",
    },
    ("vlm", "/vlm/v1/describe"): {
        "keys": (),
        "generated": ("caption",),
        "reason": "the caption is generated text, so its value is "
                  "<GENERATED>; the envelope and the string type stay compared",
    },
    ("stt", "/stt/v1/transcribe"): {
        "keys": (),
        "generated": ("text",),
        "reason": "the transcript is the recognizer's output for one second of "
                  "silence, so its value is <GENERATED>",
    },
}

PORT_RE = re.compile(r"^port\s*=\s*(\d+)\s*$", re.MULTILINE)
SERVER_CERT_RE = re.compile(r"^server_cert\s*=", re.MULTILINE)
SERVER_KEY_RE = re.compile(r"^server_key\s*=", re.MULTILINE)
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


def listener_scheme(unit):
    example = REPO_ROOT / "services" / unit / "config.toml.example"
    if not example.is_file():
        return "https"
    text = example.read_text()
    has_cert = SERVER_CERT_RE.search(text) is not None
    has_key = SERVER_KEY_RE.search(text) is not None
    if has_cert != has_key:
        declared = "server_cert" if has_cert else "server_key"
        missing = "server_key" if has_cert else "server_cert"
        raise SystemExit(f"{example} declares {declared} but not {missing}: a "
                         "half-written TLS listener, so no scheme can be chosen")
    return "https" if has_cert else "http"


def stack_base(stack, unit):
    config_path = stack / unit / "config.toml"
    if not config_path.is_file():
        return None
    port = PORT_RE.search(config_path.read_text())
    if not port:
        return None
    return f"{listener_scheme(unit)}://127.0.0.1:{port.group(1)}"


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
    CREDENTIALS.clear()
    CREDENTIALS.update(seed.get("credentials", {}))
    return seed["ids"], seed["sessions"]


def clear_sessions(stack):
    run_seeder(stack, ["--roles-clear"])


def session_killed(entry, record):
    return (entry["unit"], entry["method"], entry["route"]) in SESSION_KILLERS \
        and 200 <= record["response"]["status"] < 300


def remint_sessions(stack, auth_base, timeout):
    run_seeder(stack, ["--roles"])
    seed = json.loads((stack / "seed.json").read_text())
    refresh = seed["sessions"]
    CREDENTIALS.clear()
    CREDENTIALS.update(seed.get("credentials", {}))
    tokens = {}
    for role, value in refresh.items():
        access, _ = exchange(auth_base, value, timeout, role)
        tokens[role] = access
    return tokens


def device_headers(role):
    credential = CREDENTIALS.get(role) if role else None
    return {DEVICE_CREDENTIAL_HEADER: credential} if credential else {}


def exchange(auth_base, refresh_token, timeout, role=None):
    status, _, body = send(auth_base, "PATCH", SERVICE_REFRESH,
                           {"User-Agent": RECORDER_UA, **device_headers(role)},
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


def normalize_path(text):
    if STACK_DIR is not None:
        text = text.replace(str(STACK_DIR), "<STACK>")
    return text.replace(str(REPO_ROOT), "<ROOT>")


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
    if isinstance(value, str):
        return normalize_path(value)
    return value


def slot_id(route, ids):
    if route["methods"] == ["DELETE"]:
        return MISSING_ID
    if route["path"] == CHALLENGE_DETAILS:
        return CHALLENGE_SLOT
    if route["path"] == MODULE_DATA:
        return MODULE_DATA_ID
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


def fill_users(text, ids):
    if text is None:
        return None
    slots = {"resident": ids.get("residentUser"), "guest": ids.get("guestUser")}
    return USER_SLOT_RE.sub(
        lambda match: str(slots.get(match.group(1)) or MISSING_ID), text)


def fill_runtime(text):
    if text is None or isinstance(text, bytes):
        return text
    return RUNTIME_SLOT_RE.sub(
        lambda match: str(RUNTIME_IDS[match.group(1)]), text)


def dig(node, path):
    for part in path.split("."):
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def probe_plan(route, ids, roles):
    method = route["methods"][0]
    filters = route["filters"]
    guarded = "JwtFilter" in filters
    probes = []

    def add(name, auth=None, ua=RECORDER_UA, body=None,
            content_type="application/json", missing=False, path=None,
            modules_off=(), after=None, before=(), remember=None,
            settle=False):
        probes.append({
            "name": name,
            "auth": auth,
            "userAgent": ua,
            "body": body,
            "contentType": content_type if body is not None else None,
            "path": path or route_identity(route, ids, missing),
            "modulesOff": list(modules_off),
            "after": after,
            "before": list(before),
            "remember": remember,
            "settle": settle,
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
        add("plain", body=body,
            content_type=CONTENT_TYPE_OVERRIDES.get(
                (route["unit"], method, route["path"]), "application/json"))
    if "{1}" in route["path"] and slot_id(route, ids) != MISSING_ID:
        add("owner-missing", auth="owner" if guarded else None, body=body,
            missing=True)
    for extra in EXTRA_PROBES.get((route["unit"], method, route["path"]), ()):
        if extra["auth"] in roles and all(
                role in roles for role in SESSION_SLOT_RE.findall(extra["path"])):
            resolved = fill_users(extra["path"], ids)
            after = extra.get("after") or ()
            if isinstance(after, dict):
                after = (after,)
            hooks = lambda items: [
                {**hook, "path": fill_users(hook.get("path", resolved), ids),
                 "body": fill_users(hook.get("body"), ids)}
                for hook in items]
            extra_body = extra.get("body", body)
            add(extra["name"], auth=extra["auth"],
                body=fill_users(extra_body, ids), path=resolved,
                modules_off=extra.get("modulesOff", ()),
                after=hooks(after), before=hooks(extra.get("before") or ()),
                remember=extra.get("remember"),
                settle=extra.get("settle", False))
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


def unsent_probes(routes, bases, ids, roles):
    left = [route for route in routes if route["unit"] in unbooted(routes, bases)]
    return len(build_plan(left, ids, roles, {route["unit"]: "" for route in left}))


def report_unsent(routes, bases, ids, roles):
    print(f"{unsent_probes(routes, bases, ids, roles)} probes not sent "
          "(service not booted)")


def session_of(name, sessions):
    if name is None:
        return None
    if name not in sessions:
        raise SystemExit(f"the seed carries no '{name}' session")
    return sessions[name]


def current_session(base, role, token, timeout):
    status, _, body = send(base, "GET", "/auth/sessions",
                           {"User-Agent": RECORDER_UA,
                            "Authorization": f"Bearer {token}",
                            **device_headers(role)},
                           None, None, timeout)
    if status != 200:
        raise SystemExit(f"GET /auth/sessions answered {status} while "
                         "resolving a recorder session")
    for session in json.loads(body)["info"]["sessions"]:
        if session["current"]:
            return session["id"]
    raise SystemExit("the recorder session is missing from its own list")


def fresh_challenge(base, timeout):
    status, _, body = send(base, "POST", "/auth/device-login",
                           {"User-Agent": RECORDER_UA},
                           json.dumps({"pollHash": "0" * 64}),
                           "application/json", timeout)
    if status != 200:
        raise SystemExit(f"POST /auth/device-login answered {status} while "
                         "opening a challenge for a details probe")
    return json.loads(body)["info"]["challengeId"]


def resolve_path(path, base, sessions, timeout):
    path = fill_runtime(path)
    if CHALLENGE_SLOT in path:
        path = path.replace(CHALLENGE_SLOT, fresh_challenge(base, timeout))
    return SESSION_SLOT_RE.sub(
        lambda match: current_session(
            base, match.group(1), session_of(match.group(1), sessions),
            timeout), path)


def owner_call(base, method, path, sessions, timeout, body=None):
    headers = {"User-Agent": RECORDER_UA, "Accept": "application/json",
               "Authorization": f"Bearer {sessions['owner']}",
               **device_headers("owner")}
    return send(base, method, path, headers, body,
                "application/json" if body is not None else None, timeout)


def wait_until(condition, what):
    deadline = time.monotonic() + MODULE_WAIT_SECONDS
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(MODULE_POLL_SECONDS)
    raise SystemExit(f"timed out waiting for {what}")


def module_lifecycle(bases, module, sessions, timeout):
    status, _, body = owner_call(bases["settings"], "GET", "/modules",
                                 sessions, timeout)
    if status != 200:
        return None
    for item in json.loads(body)["info"]["modules"]:
        if item["id"] == module:
            return item["lifecycle"]
    return None


def module_gated(base, path, sessions, timeout):
    status, _, body = owner_call(base, "GET", path, sessions, timeout)
    return status == 403 and b"MODULE_DISABLED" in body


def switch_module_off(entry, bases, module, sessions, timeout):
    gate = MODULE_GATES.get((entry["baseUnit"], module))
    status, _, body = owner_call(bases["settings"], "POST",
                                 f"/modules/{module}/disable", sessions,
                                 timeout, "{}")
    if status != 200:
        raise SystemExit(f"disabling {module} answered {status} "
                         f"{body[:200]!r}")
    if gate is None:
        wait_until(lambda: module_lifecycle(bases, module, sessions, timeout)
                   == "disabled", f"{module} to be disabled")
        return
    base = bases[entry["baseUnit"]]
    wait_until(lambda: module_gated(base, gate, sessions, timeout),
               f"{entry['baseUnit']} to refuse {module} routes")


def install_module(bases, module, sessions, timeout):
    deadline = time.monotonic() + MODULE_WAIT_SECONDS
    while True:
        status, _, body = owner_call(bases["settings"], "POST",
                                     f"/modules/{module}/install", sessions,
                                     timeout, "{}")
        if status in (200, 202):
            return
        if status != 409 or b"MODULE_JOB_RUNNING" not in body \
                or time.monotonic() > deadline:
            raise SystemExit(f"enabling {module} answered {status} "
                             f"{body[:200]!r}")
        time.sleep(MODULE_POLL_SECONDS)


def switch_module_on(entry, bases, module, sessions, timeout):
    gate = MODULE_GATES.get((entry["baseUnit"], module))
    install_module(bases, module, sessions, timeout)
    wait_until(lambda: module_lifecycle(bases, module, sessions, timeout)
               == "active", f"{module} to be active again")
    if gate is None:
        return
    base = bases[entry["baseUnit"]]
    wait_until(lambda: not module_gated(base, gate, sessions, timeout),
               f"{entry['baseUnit']} to serve {module} routes again")


def caller_config(unit):
    return tomllib.loads((STACK_DIR / unit / "config.toml").read_text())


def grpc_call(caller, target, method, payload, timeout):
    callee = caller_config(target)["internal_tls"]["address"]
    credential = caller_config(target)["grpc"][f"caller_{caller}"]
    tls_dir = Path(caller_config(caller)["internal_tls"]["dir"])
    grpcurl = shutil.which("grpcurl") or str(Path.home() / "go/bin/grpcurl")
    command = [grpcurl,
               "-cacert", str(tls_dir / "ca.pem"),
               "-cert", str(tls_dir / "cert.pem"),
               "-key", str(tls_dir / "key.pem"),
               "-import-path", str(REPO_ROOT / "packages/contracts/proto"),
               "-proto", "argus/notification/v1/notification.proto",
               "-H", f"x-argus-credential: {credential}",
               "-d", json.dumps(payload), callee, method]
    result = subprocess.run(command, capture_output=True, text=True,
                            timeout=timeout)
    if result.returncode != 0:
        raise SystemExit(f"{method} answered {result.returncode}: "
                         f"{result.stderr[:200]!r}")
    return json.loads(result.stdout or "{}")


class SyncSocket:
    def __init__(self, base, token, role, timeout):
        host, port = base.split("://", 1)[1].rsplit(":", 1)
        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        raw = socket.create_connection((host, int(port)), timeout=timeout)
        self.conn = context.wrap_socket(raw, server_hostname=host)
        self.conn.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        lines = ["GET /sync HTTP/1.1", f"Host: {host}:{port}",
                 "Upgrade: websocket", "Connection: Upgrade",
                 f"Sec-WebSocket-Key: {key}", "Sec-WebSocket-Version: 13",
                 f"Authorization: Bearer {token}",
                 f"User-Agent: {RECORDER_UA}",
                 *(f"{name}: {value}" for name, value
                   in device_headers(role).items())]
        self.conn.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.conn.recv(4096)
            if not chunk:
                raise SystemExit("sync closed the socket during the handshake")
            head += chunk
        status_line, _, rest = head.partition(b"\r\n")
        if b" 101 " not in status_line:
            raise SystemExit(f"sync refused the socket: {status_line!r}")
        self.buffer = bytearray(rest.partition(b"\r\n\r\n")[2])

    def _fill(self, size):
        while len(self.buffer) < size:
            chunk = self.conn.recv(65536)
            if not chunk:
                raise SystemExit("the sync socket closed")
            self.buffer.extend(chunk)

    def frame(self):
        self._fill(2)
        opcode, length = self.buffer[0] & 0x0F, self.buffer[1] & 0x7F
        offset = 2
        if length == 126:
            self._fill(4)
            length, offset = struct.unpack(">H", bytes(self.buffer[2:4]))[0], 4
        elif length == 127:
            self._fill(10)
            length, offset = struct.unpack(">Q", bytes(self.buffer[2:10]))[0], 10
        self._fill(offset + length)
        payload = bytes(self.buffer[offset:offset + length])
        del self.buffer[:offset + length]
        return opcode, payload

    def await_operation(self, operation, deadline):
        while time.monotonic() < deadline:
            try:
                opcode, payload = self.frame()
            except socket.timeout:
                return None
            if opcode == 0x9:
                self.conn.sendall(b"\x8a\x80\x00\x00\x00\x00")
                continue
            if opcode == 0x1:
                info = find_operation(json.loads(payload), operation)
                if info is not None:
                    return info
        return None

    def close(self):
        self.conn.close()


def find_operation(node, operation):
    if isinstance(node, dict):
        if node.get("operation") == operation and isinstance(node.get("info"), dict):
            return node["info"]
        for value in node.values():
            found = find_operation(value, operation)
            if found is not None:
                return found
    if isinstance(node, list):
        for value in node:
            found = find_operation(value, operation)
            if found is not None:
                return found
    return None


def ring_hook(hook, bases, sessions, timeout):
    token = session_of("owner", sessions)
    user = OWNER_ID
    socket_ = SyncSocket(bases["sync"], token, "owner", timeout)
    window_end = time.monotonic() + RING_WINDOW_SECONDS
    info = None
    try:
        if socket_.await_operation(INITIAL_INFO_OPERATION,
                                   time.monotonic() + timeout) is None:
            raise SystemExit("sync sent no InitialInfo to the recorder socket")
        while info is None:
            token_key = secrets.token_hex(8)
            data = {**hook["data"], "threadKey": f"guard:panic:golden-{token_key}"}
            created = grpc_call(hook["caller"], hook["target"], hook["method"],
                                {**hook["request"], "user_ids": [user],
                                 "data": json.dumps(data),
                                 "command_id": f"panic:golden-{token_key}"},
                                timeout).get("created", 0)
            if int(created) != 1:
                raise SystemExit(f"the panic notice created {created} rows, "
                                 "not the one that rings the owner")
            info = socket_.await_operation(RING_OPERATION,
                                           time.monotonic() + RING_ATTEMPT_SECONDS)
            if info is None and time.monotonic() >= window_end:
                raise SystemExit("the owner's ring window stayed open for "
                                 f"{RING_WINDOW_SECONDS:.0f}s: no notice rang "
                                 "the owner, so the decline has no call")
            if info is None:
                time.sleep(RING_RETRY_SECONDS)
    finally:
        socket_.close()
    if not info.get("callId"):
        raise SystemExit("the CallIncoming frame carried no callId")
    RUNTIME_IDS[hook["remember"]] = info["callId"]


def run_hook(hook, entry, bases, sessions, timeout):
    if "ring" in hook:
        return ring_hook(hook["ring"], bases, sessions, timeout)
    base = bases[hook.get("unit", entry["baseUnit"])]
    role = hook.get("auth", "owner")
    body = fill_runtime(hook.get("body"))
    headers = {"User-Agent": RECORDER_UA, "Accept": "application/json",
               "Authorization": f"Bearer {session_of(role, sessions)}",
               **device_headers(role)}
    path = fill_runtime(hook["path"])
    status, _, raw = send(base, hook["method"], path, headers, body,
                          "application/json" if body is not None else None,
                          timeout)
    if status >= 400 and not hook.get("tolerate"):
        raise SystemExit(f"setup call {hook['method']} {path} answered "
                         f"{status} {raw[:200]!r}")
    if hook.get("remember") and 200 <= status < 300:
        slot, where = hook["remember"]
        RUNTIME_IDS[slot] = dig(json.loads(raw), where)


def prime_guard_feed(entry):
    if (entry["unit"], entry["route"]) not in PRIMED_ROUTES:
        return
    spec = importlib.util.spec_from_file_location(
        "seed_golden", REPO_ROOT / "scripts" / "seed-golden.py")
    seeder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(seeder)
    seeder.publish_guard_heartbeat(STACK_DIR)


def run_entry(entry, bases, sessions, timeout):
    prime_guard_feed(entry)
    base = bases[entry["baseUnit"]]
    probe = entry["probe"]
    off = probe.get("modulesOff") or []
    before = probe.get("before") or []
    after = probe.get("after") or []
    if isinstance(after, dict):
        after = [after]
    if not off and not before and not after:
        return run_probe(entry, base, sessions, timeout)
    if off and "settings" not in bases:
        raise SystemExit("a probe that switches a module off needs "
                         "argus-settings booted")
    switched = []
    try:
        for hook in before:
            run_hook(hook, entry, bases, sessions, timeout)
        for module in off:
            switch_module_off(entry, bases, module, sessions, timeout)
            switched.append(module)
        if probe.get("settle"):
            time.sleep(SETTLE_SECONDS)
        return run_probe(entry, base, sessions, timeout)
    finally:
        for hook in after:
            run_hook({"tolerate": True, **hook}, entry, bases, sessions,
                     timeout)
        for module in reversed(switched):
            switch_module_on(entry, bases, module, sessions, timeout)


def run_probe(entry, base, sessions, timeout):
    probe = entry["probe"]
    headers = {"User-Agent": probe["userAgent"], "Accept": "application/json"}
    token = session_of(probe["auth"], sessions)
    if token:
        headers["Authorization"] = f"Bearer {token}"
        if probe["userAgent"] == RECORDER_UA:
            headers.update(device_headers(probe["auth"]))
    payload = fill_runtime(probe["body"])
    if probe["body"] is None:
        described = None
    elif isinstance(probe["body"], bytes):
        described = f"<{len(probe['body'])} bytes>"
    else:
        described = probe["body"]
    status, content_type, raw = send(
        base, entry["method"], resolve_path(probe["path"], base, sessions,
                                            timeout),
        headers, payload, probe["contentType"], timeout)
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
            **({"modulesOff": probe["modulesOff"]}
               if probe.get("modulesOff") else {}),
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
        generated_text = ((entry["unit"], entry["route"]) in GENERATED_TEXT_ROUTES
                          and 200 <= status < 300)
        record["response"]["text"] = (GENERATED if generated_text
                                      else mask_text(body)[:TEXT_LIMIT])
    else:
        record["response"]["json"] = normalize(parsed)
        if probe.get("remember") and 200 <= status < 300:
            slot, where = probe["remember"]
            RUNTIME_IDS[slot] = dig(parsed, where)
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
    absent = ABSENT_DEPENDENCY_ANSWERS.get((entry["unit"], entry["route"]))
    if absent and status == absent[0] and error_code(record) != absent[1]:
        problems.append(f"{entry['route']} answered {status} "
                        f"{error_code(record)} instead of {absent[1]}")
    elif status >= 500 and not (status == 502 and control) and \
            not (absent and status == absent[0]):
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


def write_manifest(fixtures, units, probe_count, deviations):
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
             **({"placeholders": list(fields["placeholders"])}
                if fields.get("placeholders") else {}),
            **({"generated": list(fields["generated"])}
                if fields.get("generated") else {}),
            **({"generatedText": True} if fields.get("generatedText") else {}),
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
            "a volatile key is recorded as <VOLATILE:type>: its key and its "
            "JSON type stay compared, its value does not",
        ],
        "declaredDeviations": deviations,
        "units": units,
        "probes": probe_count,
    }
    (fixtures / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")


def recorded_units(fixtures):
    return sorted(path.stem for path in fixtures.glob("*.json")
                  if path.stem != "manifest")


def dropped_units(fixtures, bases):
    return sorted(unit for unit in recorded_units(fixtures)
                  if unit != "shared" and unit not in bases)


def only_pairs(values):
    pairs = set()
    for value in values:
        method, _, path = value.partition(" ")
        if not method or not path.startswith("/"):
            raise SystemExit(f"--only expects 'METHOD /path', got {value!r}")
        pairs.add((method, path))
    return pairs


def merge_records(fixtures, unit, records):
    path = fixture_path(fixtures, unit)
    existing = json.loads(path.read_text())["probes"] if path.is_file() else []
    position = {(row["method"], row["path"], row["probe"]): index
                for index, row in enumerate(existing)}
    for record_entry in records:
        key = (record_entry["method"], record_entry["path"],
               record_entry["probe"])
        if key in position:
            existing[position[key]] = record_entry
        else:
            existing.append(record_entry)
    path.write_text(json.dumps({"unit": unit, "probes": existing},
                               indent=2, sort_keys=True) + "\n")


def recorded_probe_count(fixtures):
    return sum(len(json.loads(fixture_path(fixtures, unit).read_text())["probes"])
               for unit in recorded_units(fixtures))


def record(fixtures, routes, bases, sessions, ids, roles, timeout, stack,
           only=None):
    unknown = unbooted(routes, bases)
    for unit in unknown:
        print(f"skip: {unit} is not booted in this stack")
    report_unsent(routes, bases, ids, roles)
    dropped = [] if only is not None else dropped_units(fixtures, bases)
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
    if only is not None:
        planned = {(entry["method"], entry["route"]) for entry in plan}
        absent = sorted(only - planned)
        if absent:
            print("refusing to record: the stack plans no row for "
                  + ", ".join(f"{method} {path}" for method, path in absent),
                  file=sys.stderr)
            return 2
        plan = [entry for entry in plan
                if (entry["method"], entry["route"]) in only]
    failures = 0
    units = {}
    fixtures.mkdir(parents=True, exist_ok=True)
    for entry in plan:
        record_entry = run_entry(entry, bases, sessions, timeout)
        problems = violates(entry, record_entry, deviations)
        for problem in problems:
            failures += 1
            print(f"violation: {entry['unit']} {entry['method']} "
                  f"{entry['route']} [{entry['probe']['name']}] {problem}",
                  file=sys.stderr)
        record_entry["response"]["json"] = without_volatile(
            entry, record_entry["response"]["json"], VOLATILE_FIELDS)
        units.setdefault(entry["unit"], []).append(record_entry)
        if session_killed(entry, record_entry):
            sessions = remint_sessions(stack, bases["auth"], timeout)
    for unit, records in sorted(units.items()):
        if only is None:
            fixture_path(fixtures, unit).write_text(
                json.dumps({"unit": unit, "probes": records},
                           indent=2, sort_keys=True) + "\n")
        else:
            merge_records(fixtures, unit, records)
    if only is None:
        write_manifest(fixtures, sorted(units), len(plan), deviations)
    else:
        write_manifest(fixtures, recorded_units(fixtures),
                       recorded_probe_count(fixtures), deviations)
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


GENERATED_TEXT_ROUTES = {key for key, fields in VOLATILE_FIELDS.items()
                         if fields.get("generatedText")}


def without_volatile(entry, body, volatile):
    fields = volatile.get((entry["unit"], entry["route"]))
    if not fields or not isinstance(body, dict):
        return body
    if "info" in body and not isinstance(body["info"], dict):
        return body
    body = json.loads(json.dumps(body))
    node = body["info"] if "info" in body else body
    for key in fields["keys"]:
        replace_path(node, key.split("."), volatile_marker)
    for key in fields.get("placeholders", ()):
        replace_path(node, key.split("."), lambda _: PLACEHOLDER)
    for key in fields.get("generated", ()):
        replace_path(node, key.split("."), lambda _: GENERATED)
    return body


def json_type(value):
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, (int, float)):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    return "object"


def volatile_marker(value):
    return f"<VOLATILE:{json_type(value)}>"


def replace_path(node, parts, mark):
    head, rest = parts[0], parts[1:]
    if head.endswith("[]"):
        items = node.get(head[:-2]) if isinstance(node, dict) else None
        for item in items if isinstance(items, list) else ():
            replace_path(item, rest, mark)
        return
    if not isinstance(node, dict):
        return
    if rest:
        replace_path(node.get(head), rest, mark)
    elif head in node:
        node[head] = mark(node[head])


def compare(expected, actual, volatile):
    problems = []
    for field in ("status", "contentType", "pretty"):
        if expected["response"][field] != actual["response"][field]:
            problems.append(
                f"response.{field}: {expected['response'][field]!r} -> "
                f"{actual['response'][field]!r}")
    problems.extend(differences(
        expected["response"].get("json"),
        without_volatile(actual, actual["response"].get("json"), volatile),
        "response.json"))
    if expected["response"].get("text") != actual["response"].get("text"):
        problems.append("response.text changed")
    return problems


def verify(fixtures, routes, bases, sessions, ids, roles, timeout, verbose,
           stack):
    for unit in unbooted(routes, bases):
        print(f"skip: {unit} is not booted in this stack")
    report_unsent(routes, bases, ids, roles)
    manifest = load_manifest(fixtures)
    deviations = manifest["declaredDeviations"]
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
        actual = run_entry(entry, bases, sessions, timeout)
        problems = compare(expected, actual, VOLATILE_FIELDS)
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


def recorded_routes(fixtures):
    recorded = set()
    for path in sorted(fixtures.glob("*.json")):
        if path.stem == "manifest":
            continue
        for row in json.loads(path.read_text())["probes"]:
            recorded.add((row["method"], row["route"]))
    return recorded


def census(routes, fixtures):
    bases = {route["unit"]: "" for route in routes}
    plan = build_plan(routes, dict(CENSUS_IDS), ["owner", "resident", "guest"],
                      bases)
    for unit, entries in sorted(group_by_unit(plan).items()):
        print(f"{unit}\t{len(entries)} probes\t"
              f"{len({entry['route'] for entry in entries})} routes")
    print(f"census: {len(plan)} probes over {len(group_by_unit(plan))} units, "
          "planned against every unit the census declares")
    recorded = recorded_routes(fixtures)
    owners = {(entry["method"], entry["route"]): entry["unit"] for entry in plan}
    missing = sorted((unit, method, route)
                     for (method, route), unit in owners.items()
                     if (method, route) not in recorded)
    for unit, method, route in missing:
        print(f"unrecorded census route: {unit} {method} {route}",
              file=sys.stderr)
    if missing:
        print(f"census: {len(missing)} census routes have no recording in "
              f"{fixtures}", file=sys.stderr)
        return 1
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
    parser.add_argument("--only", action="append", default=[],
                        metavar="METHOD /path",
                        help="record mode only: re-record the rows of this "
                             "route and leave every other row byte for byte "
                             "(repeatable)")
    return parser.parse_args(argv)


def main(argv):
    global STACK_DIR, OWNER_ID
    args = parse_args(argv)
    if args.only and args.command != "record":
        print("--only applies to record only", file=sys.stderr)
        return 2
    routes = parse_baseline(Path(args.baseline))
    if args.command == "census":
        return census(routes, Path(args.fixtures))

    stack = Path(args.stack_dir)
    STACK_DIR = stack
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
    OWNER_ID = ids.get("ownerUser")
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
        access, _ = exchange(bases["auth"], refresh[role], args.timeout, role)
        sessions[role] = access
    try:
        if args.command == "record":
            return record(fixtures, routes, bases, sessions, ids, roles,
                          args.timeout, stack,
                          only_pairs(args.only) if args.only else None)
        return verify(fixtures, routes, bases, sessions, ids, roles,
                      args.timeout, args.verbose, stack)
    finally:
        if not args.no_mint:
            clear_sessions(stack)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
