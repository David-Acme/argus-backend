#!/usr/bin/env python3
import argparse
import json
import pathlib
import shlex
import statistics
import subprocess
import sys
import time
import unicodedata

SKIP = 77
REQUIRED = {
    "calendar.create_event": ["title", "starts_at"],
    "calendar.list_events": [],
    "calendar.cancel_event": [],
    "task.create": ["title"],
    "task.complete": [],
    "project.create": ["name"],
    "modules.explain": ["module"],
    "modules.enable": ["module"],
    "modules.disable": ["module"],
    "modules.open_purge_screen": ["module"],
}


def fold(text):
    stripped = unicodedata.normalize("NFD", str(text).lower())
    return "".join(ch for ch in stripped if unicodedata.category(ch) != "Mn")


def minute(value):
    text = str(value)
    return text[:16] if "T" in text else text


def matches(matcher, value):
    if value is None:
        return False
    if "equals" in matcher and minute(value) != minute(matcher["equals"]):
        return False
    if "startsWith" in matcher and not str(value).startswith(matcher["startsWith"]):
        return False
    folded = fold(value)
    if any(fold(needle) not in folded for needle in matcher.get("contains", [])):
        return False
    return not any(fold(needle) in folded for needle in matcher.get("notContains", []))


def score_case(case, answer):
    expect = case["expect"]
    args = {k: v for k, v in (answer.get("args") or {}).items() if v not in (None, "")}
    reported = sorted(answer.get("missing") or [])
    result = {"id": case["id"], "tool": case["tool"], "variant": case["variant"], "args": {}}
    if expect["missing"]:
        result["missingCorrect"] = reported == sorted(expect["missing"])
        result["guessed"] = any(name in args for name in expect["missing"])
        result["argsCorrect"] = all(matches(m, args.get(n)) for n, m in expect["args"].items())
        result["correct"] = result["missingCorrect"] and not result["guessed"] and result["argsCorrect"]
        return result
    result["spuriousClarify"] = bool(reported)
    for name, matcher in expect["args"].items():
        result["args"][name] = matches(matcher, args.get(name))
    result["correct"] = all(result["args"].values()) and not reported
    return result


def rate(results, key, select=None):
    pool = [r[key] for r in results if key in r and (select is None or select(r))]
    return (sum(1 for v in pool if v) / len(pool), len(pool)) if pool else None


def metrics_of(results):
    values = {}
    resolvable = [r for r in results if "spuriousClarify" in r]
    if resolvable:
        values["slotAccuracy"] = sum(r["correct"] for r in resolvable) / len(resolvable)
        values["spuriousClarifyRate"] = sum(r["spuriousClarify"] for r in resolvable) / len(resolvable)
    for arg, name in (("starts_at", "timeAccuracy"), ("title", "titleAccuracy"), ("name", "nameAccuracy"),
                      ("module", "moduleAccuracy"), ("from", "rangeAccuracy"), ("due_at", "dueAccuracy")):
        pool = [r["args"][arg] for r in resolvable if arg in r["args"]]
        if pool:
            values[name] = sum(pool) / len(pool)
    asked = [r for r in results if "missingCorrect" in r]
    if asked:
        values["missingAccuracy"] = sum(r["missingCorrect"] for r in asked) / len(asked)
        values["guessedRate"] = sum(r["guessed"] for r in asked) / len(asked)
    for variant in sorted({r["variant"] for r in results}):
        pool = [r for r in resolvable if r["variant"] == variant]
        if pool:
            values[f"variant.{variant}.slotAccuracy"] = sum(r["correct"] for r in pool) / len(pool)
    for tool in sorted({r["tool"] for r in results}):
        pool = [r for r in resolvable if r["tool"] == tool]
        if pool:
            values[f"tool.{tool}.slotAccuracy"] = sum(r["correct"] for r in pool) / len(pool)
    return values


def check_gates(gates, values):
    failures = []
    for name, bound in gates.get("slots", {}).get("metrics", {}).items():
        if name not in values:
            continue
        if "max" in bound and values[name] > bound["max"]:
            failures.append(f"{name}: {values[name]:.4f} above {bound['max']}")
        if "min" in bound and values[name] < bound["min"]:
            failures.append(f"{name}: {values[name]:.4f} below {bound['min']}")
    return failures


def fill_all(command, cases):
    try:
        process = subprocess.Popen(shlex.split(command), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   text=True, bufsize=1)
    except OSError as error:
        print(f"slot-eval: cannot start the filler: {error}")
        return None, None
    answers, times = {}, []
    for case in cases:
        request = {"seq": case["id"], "user": case["user"], "lang": case["lang"], "tool": case["tool"],
                   "now": case["now"], "required": REQUIRED.get(case["tool"], [])}
        started = time.perf_counter()
        process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        process.stdin.flush()
        line = process.stdout.readline()
        times.append((time.perf_counter() - started) * 1000.0)
        if not line:
            raise SystemExit("the filler closed its output before answering every request")
        answer = json.loads(line)
        answers[answer["seq"]] = answer
    process.stdin.close()
    process.wait(timeout=60)
    return answers, times


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", required=True)
    parser.add_argument("--gates", required=True)
    parser.add_argument("--filler", required=True)
    parser.add_argument("--report")
    args = parser.parse_args()
    gates = json.loads(pathlib.Path(args.gates).read_text())
    cases = [json.loads(line) for line in pathlib.Path(args.cases).read_text().splitlines() if line.strip()]
    answers, times = fill_all(args.filler, cases)
    if answers is None:
        return SKIP
    results = [score_case(case, answers[case["id"]]) for case in cases]
    values = metrics_of(results)
    print(f"slots: {len(results)} cases")
    for name in sorted(values):
        if "." not in name:
            print(f"  {name:22s}{values[name]:8.3f}")
    print("  by variant / tool:")
    for name in sorted(values):
        if "." in name:
            print(f"    {name:48s}{values[name]:8.3f}")
    ordered = sorted(times)
    latency = {"p50": statistics.median(times), "p95": ordered[max(0, int(len(ordered) * 0.95) - 1)]}
    print(f"  filler latency p50 {latency['p50']:.1f} ms, p95 {latency['p95']:.1f} ms")
    failures = check_gates(gates, values)
    if args.report:
        pathlib.Path(args.report).write_text(json.dumps({"metrics": values, "latencyMs": latency,
                                                         "failures": failures}, indent=2, sort_keys=True) + "\n")
    if failures:
        print("GATE FAILED")
        for failure in failures:
            print("  " + failure)
        return 1
    print("GATE PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
