#!/usr/bin/env python3
import argparse
import json
import pathlib
import re
import shlex
import statistics
import subprocess
import sys
import time
import unicodedata

SKIP = 77
WRITE_KINDS = {"done", "executed"}
READ_KINDS = {"listing", "empty"}
ES_WORDS = {"el", "la", "los", "las", "de", "que", "y", "en", "un", "una", "es", "no", "para", "con", "por",
            "se", "lo", "te", "me", "mi", "tu", "su", "al", "del", "esta", "hay", "tienes", "tengo", "ya",
            "pero", "como", "si", "muy", "le", "les", "son", "estoy", "puedo", "quieres", "listo"}
EN_WORDS = {"the", "a", "an", "and", "is", "are", "to", "of", "in", "it", "you", "your", "my", "for", "on",
            "with", "that", "this", "have", "has", "i", "do", "not", "be", "was", "will", "can", "all", "set",
            "there", "what", "which", "would", "like", "me", "am", "it's", "i'm"}
NAME_WHITELIST = {"argus", "i", "lima", "peru", "perú", "ok", "monday", "tuesday", "wednesday", "thursday",
                  "friday", "saturday", "sunday", "january", "february", "march", "april", "may", "june",
                  "july", "august", "september", "october", "november", "december", "listo", "claro", "vale",
                  "perfecto", "hola", "gracias", "sí", "si", "no", "entendido", "de", "nada"}
DEFAULT_MAX_WORDS = 45
DEFAULT_MAX_SENTENCES = 4


def fold(text):
    stripped = unicodedata.normalize("NFD", text.lower().replace("’", "'"))
    return "".join(ch for ch in stripped if unicodedata.category(ch) != "Mn")


def sentences_of(text):
    return [part.strip() for part in re.split(r"[.!?\n]+", text) if part.strip()]


def contains_any(text, markers):
    return any(marker in text for marker in markers)


def compile_claims(claims):
    return {name: [re.compile(pattern) for pattern in patterns] for name, patterns in claims.items()}


def matches_any(patterns, sentence):
    return any(pattern.search(sentence) for pattern in patterns)


def claims_done(reply, kind, tool, scoring, claims):
    written = kind in WRITE_KINDS and tool in scoring["writeTools"]
    app_ran = kind in WRITE_KINDS and (tool or "").startswith("app.")
    any_ran = kind in WRITE_KINDS or kind in READ_KINDS
    for sentence in sentences_of(fold(reply)):
        if contains_any(sentence, scoring["notAClaimMarkers"]):
            continue
        if matches_any(claims["strongWrite"], sentence) and not written:
            return True
        if kind not in READ_KINDS and matches_any(claims["weakWrite"], sentence) and not written:
            return True
        if contains_any(sentence, scoring["appClaimMarkers"]) and not app_ran:
            return True
        if matches_any(claims["generalAnchored"], sentence.strip()) and not any_ran:
            return True
    return False


def flatten(value):
    if isinstance(value, dict):
        return " ".join(flatten(v) for v in value.values())
    if isinstance(value, list):
        return " ".join(flatten(v) for v in value)
    return "" if value is None else str(value)


def allowed_numbers(case):
    pool = case["user"] + " " + flatten(case["turn"].get("facts")) + " " + case["turn"].get("text", "")
    numbers = set(re.findall(r"\d+", pool))
    for match in re.finditer(r"(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})", pool):
        year, month, day, hour, minute = match.groups()
        hour_value = int(hour)
        numbers |= {str(int(day)), day, str(int(month)), month, year, hour, str(hour_value),
                    str(hour_value % 12 or 12), minute, str(int(minute))}
    numbers |= {str(n) for n in case["expect"].get("allowNumbers", [])}
    return numbers


def ungrounded(reply, case):
    pool = fold(case["user"] + " " + flatten(case["turn"].get("facts")) + " " + case["turn"].get("text", ""))
    numbers = allowed_numbers(case)
    bad = []
    for token in re.findall(r"\d+", reply):
        if token not in numbers and str(int(token)) not in numbers:
            bad.append(token)
    for sentence in sentences_of(reply):
        words = re.findall(r"[A-Za-zÁÉÍÓÚÑáéíóúñ']+", sentence)
        for word in words[1:]:
            if word[0].isupper() and fold(word) not in pool and fold(word) not in {fold(n) for n in NAME_WHITELIST}:
                bad.append(word)
    return bad


def detect_language(reply):
    words = re.findall(r"[a-záéíóúñ']+", reply.lower())
    es = sum(1 for w in words if w in ES_WORDS)
    en = sum(1 for w in words if w in EN_WORDS)
    if es + en < 2 or es == en:
        return None
    return "es" if es > en else "en"


def score_case(case, reply, scoring, claims):
    expect = case["expect"]
    kind = case["turn"]["kind"]
    folded = fold(reply)
    result = {"id": case["id"], "kind": kind, "variant": case["variant"], "lang": case["lang"]}
    result["falseCompletion"] = claims_done(reply, kind, case["turn"].get("tool"), scoring, claims)
    groups = expect.get("mentions", [])
    result["mentions"] = all(any(fold(option) in folded for option in group) for group in groups)
    result["forbidden"] = any(fold(item) in folded for item in expect.get("forbidden", []))
    if expect.get("question"):
        result["question"] = "?" in reply
    bad = ungrounded(reply, case)
    result["ungrounded"] = bool(bad)
    detected = detect_language(reply)
    result["language"] = detected is None or detected == case["lang"]
    words = len(reply.split())
    result["length"] = (words <= expect.get("maxWords", DEFAULT_MAX_WORDS)
                        and len(sentences_of(reply)) <= expect.get("maxSentences", DEFAULT_MAX_SENTENCES))
    result["words"] = words
    return result


def rate(results, key, select=None):
    pool = [r[key] for r in results if key in r and (select is None or select(r))]
    return (sum(1 for v in pool if v) / len(pool), len(pool)) if pool else (0.0, 0)


BASE_METRICS = ("falseCompletionRate", "forbiddenRate", "ungroundedRate", "mentionRate", "languageRate",
                "lengthRate", "questionRate")


def metrics_of(results):
    values = {}
    for name, key in (("falseCompletionRate", "falseCompletion"), ("forbiddenRate", "forbidden"),
                      ("ungroundedRate", "ungrounded"), ("mentionRate", "mentions"),
                      ("languageRate", "language"), ("lengthRate", "length"), ("questionRate", "question")):
        value, count = rate(results, key)
        if count:
            values[name] = value
    for scope in sorted({r["variant"] for r in results}):
        pool = [r for r in results if r["variant"] == scope]
        values[f"variant.{scope}.falseCompletionRate"] = rate(pool, "falseCompletion")[0]
        values[f"variant.{scope}.mentionRate"] = rate(pool, "mentions")[0]
    for scope in sorted({r["kind"] for r in results}):
        pool = [r for r in results if r["kind"] == scope]
        values[f"kind.{scope}.falseCompletionRate"] = rate(pool, "falseCompletion")[0]
        values[f"kind.{scope}.mentionRate"] = rate(pool, "mentions")[0]
    return values


def check_gates(gates, values):
    failures = []
    for name, bound in gates.get("conversation", {}).get("metrics", {}).items():
        if name not in values:
            if name not in BASE_METRICS and not name.startswith(("variant.", "kind.")):
                failures.append(f"{name}: no such metric")
            continue
        if "max" in bound and values[name] > bound["max"]:
            failures.append(f"{name}: {values[name]:.4f} above {bound['max']}")
        if "min" in bound and values[name] < bound["min"]:
            failures.append(f"{name}: {values[name]:.4f} below {bound['min']}")
    return failures


def speak_all(command, cases):
    try:
        process = subprocess.Popen(shlex.split(command), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   text=True, bufsize=1)
    except OSError as error:
        print(f"conversation-eval: cannot start the speaker: {error}")
        return None, None
    replies, times = {}, []
    for case in cases:
        request = {"seq": case["id"], "lang": case["lang"], "variant": case["variant"], "user": case["user"],
                   "turn": case["turn"]}
        started = time.perf_counter()
        process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        process.stdin.flush()
        line = process.stdout.readline()
        times.append((time.perf_counter() - started) * 1000.0)
        if not line:
            raise SystemExit("the speaker closed its output before answering every request")
        answer = json.loads(line)
        replies[answer["seq"]] = answer["reply"]
    process.stdin.close()
    process.wait(timeout=60)
    return replies, times


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", required=True)
    parser.add_argument("--gates", required=True)
    parser.add_argument("--speaker")
    parser.add_argument("--replies")
    parser.add_argument("--replies-out")
    parser.add_argument("--skip", type=int, default=0)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--report")
    args = parser.parse_args()

    gates = json.loads(pathlib.Path(args.gates).read_text())
    scoring = gates["llm"]["scoring"]
    claims = compile_claims(gates["conversation"]["claims"])
    cases = [json.loads(line) for line in pathlib.Path(args.cases).read_text().splitlines() if line.strip()]
    cases = cases[args.skip:(args.skip + args.limit) if args.limit else None]
    times = []
    if args.replies:
        replies = {}
        for line in pathlib.Path(args.replies).read_text().splitlines():
            if line.strip():
                row = json.loads(line)
                replies[row["id"]] = row["reply"]
        cases = [c for c in cases if c["id"] in replies]
    elif args.speaker:
        replies, times = speak_all(args.speaker, cases)
        if replies is None:
            return SKIP
        if args.replies_out:
            with open(args.replies_out, "a") as handle:
                for case in cases:
                    handle.write(json.dumps({"id": case["id"], "reply": replies[case["id"]]},
                                            ensure_ascii=False) + "\n")
    else:
        print("conversation-eval: give a speaker or saved replies")
        return SKIP

    results = [score_case(case, replies[case["id"]], scoring, claims) for case in cases]
    values = metrics_of(results)
    print(f"conversation: {len(results)} cases")
    for name in BASE_METRICS:
        if name in values:
            print(f"  {name:22s}{values[name]:8.3f}")
    print("  by kind (false completion / mentions):")
    for scope in sorted({r["kind"] for r in results}):
        print(f"    {scope:10s}{values[f'kind.{scope}.falseCompletionRate']:8.3f}{values[f'kind.{scope}.mentionRate']:8.3f}")
    print("  by variant (false completion / mentions):")
    for scope in sorted({r["variant"] for r in results}):
        print(f"    {scope:10s}{values[f'variant.{scope}.falseCompletionRate']:8.3f}{values[f'variant.{scope}.mentionRate']:8.3f}")
    report = {"cases": len(results), "metrics": values}
    if times:
        ordered = sorted(times)
        report["latencyMs"] = {"p50": statistics.median(times),
                               "p95": ordered[max(0, int(len(ordered) * 0.95) - 1)], "n": len(times)}
        print(f"  speak latency p50 {report['latencyMs']['p50']:.0f} ms, p95 {report['latencyMs']['p95']:.0f} ms")
    failures = check_gates(gates, values)
    report["failures"] = failures
    if args.report:
        pathlib.Path(args.report).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    if failures:
        print("GATE FAILED")
        for failure in failures:
            print("  " + failure)
        return 1
    print("GATE PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
