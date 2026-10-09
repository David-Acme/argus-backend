#!/usr/bin/env python3
import argparse
import http.client
import json
import os
import pathlib
import statistics
import sys
import time
import urllib.parse

SKIP = 77
SENTINEL = "\x1e"

READY_MARKERS = {
    "laya": "the Laya decider is ready on",
    "gliner": "the GLiNER extractor opened",
}

SYSTEM_ES = (
    "Eres Argus, el asistente de una casa. Hablas en español, en frases cortas y naturales. "
    "No eliges herramientas: solo dices lo que ocurre."
)
SYSTEM_EN = (
    "You are Argus, the assistant of a home. You speak English, in short natural sentences. "
    "You do not choose tools: you only say what happens."
)

ACT_TURNS = {
    "slot": [
        ("es", "agéndame una reunión con Andrea"),
        ("es", "anota una tarea"),
        ("en", "schedule a meeting with Andrea"),
        ("en", "note a task"),
    ],
    "confirm": [
        ("es", "cancela la cita con el dentista"),
        ("es", "apaga el módulo de productividad"),
        ("en", "cancel the dentist appointment"),
        ("en", "turn off the productivity module"),
    ],
    "choose": [
        ("es", "apunta la reunión con Andrea"),
        ("es", "apunta la cita del dentista"),
        ("en", "note the meeting with Andrea"),
        ("en", "note the dentist appointment"),
    ],
}

PLAIN_TURNS = [
    ("es", "hola, ¿cómo estás?"),
    ("es", "cuéntame algo interesante sobre los gatos"),
    ("en", "hello, how are you?"),
    ("en", "tell me something interesting about cats"),
]


def busy_fraction(seconds):
    def read():
        fields = pathlib.Path("/proc/stat").read_text().splitlines()[0].split()[1:]
        values = [int(v) for v in fields]
        return sum(values), values[3] + values[4]

    total_a, idle_a = read()
    time.sleep(seconds)
    total_b, idle_b = read()
    span = total_b - total_a
    return 1.0 - (idle_b - idle_a) / span if span else 0.0


def percentile(values, share):
    ordered = sorted(values)
    return ordered[max(0, int(len(ordered) * share) - 1)]


class Client:
    def __init__(self, url):
        parsed = urllib.parse.urlparse(url)
        self.host = parsed.hostname or "127.0.0.1"
        self.port = parsed.port or 80
        self.path = parsed.path or "/llm/v1/chat-stream"

    def headers_ready(self):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=60)
        connection.connect()
        connection.close()

    def first_byte_ms(self, lang, text, tools, tokens, session):
        body = json.dumps(
            {
                "messages": [
                    {"role": "system", "content": SYSTEM_ES if lang == "es" else SYSTEM_EN},
                    {"role": "user", "content": text},
                ],
                "max_tokens": tokens,
                "temperature": 0.3,
                "tools": tools,
                "lang": lang,
                "role": "owner",
                "user_id": 7,
                "session_id": session,
                "reset_context": False,
            },
            ensure_ascii=False,
        ).encode("utf-8")
        connection = http.client.HTTPConnection(self.host, self.port, timeout=60)
        started = time.perf_counter()
        try:
            connection.request(
                "POST",
                self.path,
                body=body,
                headers={"Content-Type": "application/json"},
            )
            response = connection.getresponse()
            if response.status != 200:
                return None, f"HTTP {response.status}"
            while True:
                chunk = response.read1(64)
                if not chunk:
                    return None, "the stream ended without a body"
                if chunk.lstrip(SENTINEL.encode("utf-8")[:1]):
                    return (time.perf_counter() - started) * 1000.0, None
        finally:
            connection.close()


def measure(args):
    client = Client(args.url)
    client.headers_ready()

    def one(lang, text, tools, kind, index):
        return client.first_byte_ms(lang, text, tools, args.max_tokens, f"ttft-{kind}-{lang}-{index % 4}")

    for kind, turns in ACT_TURNS.items():
        for index in range(min(args.warmup, len(turns))):
            lang, text = turns[index % len(turns)]
            one(lang, text, True, kind, index)

    refusal = engines_up(args.server_log, args.require_engine)
    if refusal is not None:
        return refusal

    for index in range(args.warmup // 4 + 1):
        lang, text = PLAIN_TURNS[index % len(PLAIN_TURNS)]
        one(lang, text, False, "plain", index)

    per_kind = {}
    for kind, turns in ACT_TURNS.items():
        times = []
        for index in range(args.requests):
            lang, text = turns[index % len(turns)]
            elapsed, error = one(lang, text, True, kind, index)
            if error:
                raise RuntimeError(error)
            times.append(elapsed)
        per_kind[kind] = times

    plain = []
    for index in range(args.requests):
        lang, text = PLAIN_TURNS[index % len(PLAIN_TURNS)]
        elapsed, error = one(lang, text, False, "plain", index)
        if error:
            raise RuntimeError(error)
        plain.append(elapsed)

    act = [value for times in per_kind.values() for value in times]
    values = {
        "actP50Ms": statistics.median(act),
        "actP95Ms": percentile(act, 0.95),
        "actMaxMs": max(act),
        "actMeanMs": statistics.fmean(act),
        "plainP50Ms": statistics.median(plain),
        "plainP95Ms": percentile(plain, 0.95),
        "plainMeanMs": statistics.fmean(plain),
        "requests": args.requests,
    }
    values["addedMeanMs"] = values["actMeanMs"] - values["plainMeanMs"]
    values["addedP95Ms"] = values["actP95Ms"] - values["plainP95Ms"]
    for kind, times in per_kind.items():
        values[f"{kind}.p95Ms"] = percentile(times, 0.95)
        values[f"{kind}.meanMs"] = statistics.fmean(times)
    return values


def check(gates, values):
    failures = []
    for name, bound in gates.get("speechTtft", {}).get("metrics", {}).items():
        if name not in values:
            failures.append(f"{name}: no such metric")
            continue
        if "max" in bound and values[name] > bound["max"]:
            failures.append(f"{name}: {values[name]:.2f} above {bound['max']}")
        if "min" in bound and values[name] < bound["min"]:
            failures.append(f"{name}: {values[name]:.2f} below {bound['min']}")
    return failures


def engines_up(server_log, required):
    if not required:
        return None
    if not server_log:
        return "an engine is required but no --server-log was given"
    try:
        text = pathlib.Path(server_log).read_text(errors="replace")
    except OSError as error:
        return f"the server log cannot be read: {error}"
    for name in required:
        marker = READY_MARKERS.get(name)
        if marker is None:
            return f"an unknown engine was required: {name}"
        if marker not in text:
            return f"the {name} engine is not up: no '{marker}' ready line in {server_log}"
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:7932/llm/v1/chat-stream")
    parser.add_argument("--gates")
    parser.add_argument("--requests", type=int, default=200)
    parser.add_argument("--warmup", type=int, default=20)
    parser.add_argument("--max-tokens", type=int, default=96)
    parser.add_argument("--busy-limit", type=float, default=0.20)
    parser.add_argument("--allow-busy", action="store_true")
    parser.add_argument("--label", default="speech-ttft")
    parser.add_argument("--report")
    parser.add_argument("--server-log")
    parser.add_argument("--require-engine", action="append", default=[])
    args = parser.parse_args()

    busy = busy_fraction(3.0)
    load = float(pathlib.Path("/proc/loadavg").read_text().split()[0])
    idle = busy <= args.busy_limit
    if not idle and not args.allow_busy:
        print(f"speech-ttft-eval: the machine is busy (cpu {busy:.0%}, load {load:.1f}); no number is recorded")
        return SKIP
    try:
        values = measure(args)
    except (OSError, RuntimeError, http.client.HTTPException) as error:
        print(f"speech-ttft-eval: cannot drive {args.url}: {error}")
        return SKIP
    if isinstance(values, str):
        print(f"speech-ttft-eval: refused, {values}; no gate is reported")
        return 1
    values["idle"] = 1 if idle else 0
    print(f"{args.label}: " + ("idle machine" if idle else f"BUSY machine (cpu {busy:.0%}, load {load:.1f}): NOT REPORTABLE"))
    for name in ("actP50Ms", "actP95Ms", "actMaxMs", "actMeanMs", "plainP50Ms", "plainP95Ms", "plainMeanMs",
                 "addedP95Ms", "addedMeanMs", "slot.p95Ms", "confirm.p95Ms", "choose.p95Ms"):
        print(f"  {name:16s}{values[name]:10.2f}")
    failures = []
    if args.gates:
        failures = check(json.loads(pathlib.Path(args.gates).read_text()), values)
        if not idle:
            failures.append("measured on a busy machine: the gate is not judged")
    if args.report:
        pathlib.Path(args.report).write_text(
            json.dumps({"label": args.label, "metrics": values, "failures": failures}, indent=2, sort_keys=True) + "\n"
        )
    if failures:
        print("GATE FAILED")
        for failure in failures:
            print("  " + failure)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
