#!/usr/bin/env python3
import argparse
import hashlib
import json
import math
import pathlib
import shlex
import statistics
import subprocess
import sys
import threading
import time

SKIP = 77
FAMILIES = {
    "calendar": ("calendar.create_event", "calendar.list_events", "calendar.cancel_event"),
    "task": ("task.create", "task.list", "task.complete"),
    "project": ("project.create", "project.list"),
    "modules": ("modules.list", "modules.explain", "modules.enable", "modules.request",
                "modules.disable", "modules.open_purge_screen"),
    "reminders": ("reminder.list",),
    "memory": ("memory.remember", "memory.recall", "memory.remind", "memory.forget"),
    "app": ("app.open", "app.set_guard_mode"),
    "camera": ("app.show_camera",),
}
FAMILY_OF = {tool: family for family, tools in FAMILIES.items() for tool in tools}
MODULE_FAMILIES = ("calendar", "task", "project", "modules", "reminders")
OWNER_ONLY = {"modules.enable", "modules.disable", "modules.open_purge_screen"}
MEMBER_ONLY = {"modules.request"}
THRESHOLDS = (0.0, 0.3, 0.5, 0.6, 0.7, 0.8, 0.85, 0.9, 0.93, 0.95, 0.97, 0.98, 0.99, 0.995)


def offered(role):
    tools = set(FAMILY_OF)
    if role == "owner":
        return sorted(tools - MEMBER_ONLY)
    return sorted(tools - OWNER_ONLY)


def wilson(hits, total, z=1.96):
    if total == 0:
        return 0.0, 0.0
    p = hits / total
    d = 1 + z * z / total
    centre = (p + z * z / (2 * total)) / d
    half = z * math.sqrt(p * (1 - p) / total + z * z / (4 * total * total)) / d
    return max(0.0, centre - half), min(1.0, centre + half)


def expected_tools(record):
    expect = record.get("expect", {})
    calls = [call["tool"] for call in expect.get("calls") or []]
    optional = []
    if calls:
        return set(calls), optional
    if record.get("role", "owner") != "owner":
        return None, optional
    for key, field in (("confirm", "tool"), ("inactive", "attempted"), ("offerAccept", "attempted")):
        entry = expect.get(key)
        if entry and entry.get(field):
            return {entry[field]}, optional
    if "offerDecline" in expect:
        return None, optional
    if record.get("route") == "camera":
        return set(), ["app.show_camera"]
    return set(), optional


def load_cases(paths):
    cases, seen = [], set()
    for path in paths:
        for line in pathlib.Path(path).read_text().splitlines():
            if not line.strip():
                continue
            record = json.loads(line)
            wanted, optional = expected_tools(record)
            if wanted is None:
                continue
            text = record["script"][0]
            key = (text, tuple(sorted(wanted)))
            if key in seen:
                continue
            seen.add(key)
            cases.append({"text": text, "lang": record["lang"], "role": record.get("role", "owner"),
                          "expected": wanted, "optional": optional,
                          "stratum": "real" if record.get("variant") == "real" else "authored",
                          "set": pathlib.Path(path).stem})
    return cases


def load_negatives(path):
    cases = []
    for line in pathlib.Path(path).read_text().splitlines():
        text = line.strip()
        if text:
            cases.append({"text": text, "lang": "es", "role": "owner", "expected": set(), "optional": [],
                          "stratum": "real", "set": pathlib.Path(path).stem})
    return cases


class Decider:
    def __init__(self, command):
        self.process = subprocess.Popen(shlex.split(command), stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True, bufsize=1)
        self.violations = 0

    def request(self, seq, case):
        return json.dumps({"seq": seq, "text": case["text"], "lang": case["lang"],
                           "role": case["role"], "tools": offered(case["role"])},
                          ensure_ascii=False)

    def decide_all(self, cases):
        def feed():
            for seq, case in enumerate(cases):
                self.process.stdin.write(self.request(seq, case) + "\n")
            self.process.stdin.flush()

        writer = threading.Thread(target=feed)
        writer.start()
        answers = {}
        while len(answers) < len(cases):
            line = self.process.stdout.readline()
            if not line:
                raise SystemExit("decider closed its output before answering every request")
            reply = json.loads(line)
            answers[reply["seq"]] = (reply.get("tool"), float(reply.get("confidence", 0.0)))
        writer.join()
        decisions = []
        for seq, case in enumerate(cases):
            tool, confidence = answers[seq]
            if tool is not None and tool not in offered(case["role"]):
                self.violations += 1
                tool, confidence = None, 0.0
            decisions.append((tool, confidence))
        return decisions

    def latencies(self, cases):
        out = []
        for seq, case in enumerate(cases):
            started = time.perf_counter()
            self.process.stdin.write(self.request(seq, case) + "\n")
            self.process.stdin.flush()
            self.process.stdout.readline()
            out.append((time.perf_counter() - started) * 1000.0)
        return out

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=60)


def score(cases, decisions, threshold):
    stats = {family: {"positives": 0, "routed": 0, "correct": 0, "others": 0, "falseRoute": 0,
                      "authoredOthers": 0, "authoredFalse": 0}
             for family in FAMILIES}
    pooled = {"positives": 0, "routed": 0, "correct": 0, "others": 0, "falseRoute": 0, "wrong": 0,
              "authoredOthers": 0, "authoredFalse": 0, "realOthers": 0, "realFalse": 0,
              "destructiveOthers": 0, "destructiveFalse": 0}
    for case, (tool, confidence) in zip(cases, decisions):
        if tool is not None and confidence < threshold:
            tool = None
        wanted = case["expected"]
        families = {FAMILY_OF[t] for t in wanted if t in FAMILY_OF}
        decided = FAMILY_OF.get(tool) if tool else None
        is_module_positive = bool(families & set(MODULE_FAMILIES))
        for family in FAMILIES:
            entry = stats[family]
            if family in families:
                entry["positives"] += 1
                if tool in wanted and decided == family:
                    entry["correct"] += 1
            else:
                entry["others"] += 1
                authored = case["stratum"] != "real"
                entry["authoredOthers"] += authored
                if decided == family and tool not in case["optional"]:
                    entry["falseRoute"] += 1
                    entry["authoredFalse"] += authored
            if decided == family:
                entry["routed"] += 1
        if is_module_positive:
            pooled["positives"] += 1
            if tool in wanted:
                pooled["correct"] += 1
            elif decided in MODULE_FAMILIES:
                pooled["wrong"] += 1
        else:
            pooled["others"] += 1
            stratum = "real" if case["stratum"] == "real" else "authored"
            pooled[stratum + "Others"] += 1
            if decided in MODULE_FAMILIES:
                pooled["falseRoute"] += 1
                pooled[stratum + "False"] += 1
        if decided in MODULE_FAMILIES:
            pooled["routed"] += 1
    return stats, pooled


def rate(hits, total):
    return hits / total if total else 0.0


def summarise(cases, decisions, threshold):
    stats, pooled = score(cases, decisions, threshold)
    out = {"threshold": threshold, "cases": len(cases), "families": {}}
    for family, entry in stats.items():
        out["families"][family] = {
            "positives": entry["positives"], "routed": entry["routed"],
            "coverage": rate(entry["correct"], entry["positives"]),
            "precision": rate(entry["correct"], entry["routed"]),
            "falseRouteRate": rate(entry["falseRoute"], entry["others"]),
            "falseRoute": entry["falseRoute"], "others": entry["others"],
            "falseRouteUpper": wilson(entry["falseRoute"], entry["others"])[1],
            "authoredFalseRouteRate": rate(entry["authoredFalse"], entry["authoredOthers"])}
    out["moduleFamilies"] = {
        "positives": pooled["positives"], "routed": pooled["routed"],
        "coverage": rate(pooled["correct"], pooled["positives"]),
        "precision": rate(pooled["correct"], pooled["routed"]),
        "wrongTool": pooled["wrong"],
        "falseRoute": pooled["falseRoute"], "others": pooled["others"],
        "falseRouteRate": rate(pooled["falseRoute"], pooled["others"]),
        "falseRouteUpper": wilson(pooled["falseRoute"], pooled["others"])[1],
        "authoredFalseRouteRate": rate(pooled["authoredFalse"], pooled["authoredOthers"]),
        "authoredFalseRouteUpper": wilson(pooled["authoredFalse"], pooled["authoredOthers"])[1],
        "authoredOthers": pooled["authoredOthers"],
        "realFalseRouteRate": rate(pooled["realFalse"], pooled["realOthers"]),
        "realFalseRouteUpper": wilson(pooled["realFalse"], pooled["realOthers"])[1],
        "realOthers": pooled["realOthers"]}
    return out


def rates_of(summary):
    pooled = summary["moduleFamilies"]
    values = {"pooled": pooled["falseRouteRate"], "pooled authored": pooled["authoredFalseRouteRate"]}
    for family in MODULE_FAMILIES:
        values[family] = summary["families"][family]["falseRouteRate"]
        values[family + " authored"] = summary["families"][family]["authoredFalseRouteRate"]
    return values


def passes(summary, ceiling):
    return all(value <= ceiling for value in rates_of(summary).values())


def choose_threshold(cases, decisions, ceiling):
    for threshold in THRESHOLDS:
        if passes(summarise(cases, decisions, threshold), ceiling):
            return threshold
    return None


def print_sweep(title, cases, decisions):
    print(f"\n{title}: {len(cases)} cases")
    print(f"  {'thr':>6s}{'cover':>8s}{'prec':>8s}{'wrong':>7s}{'falseR':>8s}{'rate':>8s}{'up95':>8s}"
          f"{'authored':>10s}{'real':>8s}  per-family false-route")
    for threshold in THRESHOLDS:
        s = summarise(cases, decisions, threshold)
        m = s["moduleFamilies"]
        per = " ".join(f"{f[:4]}={s['families'][f]['falseRouteRate']:.2%}" for f in MODULE_FAMILIES)
        print(f"  {threshold:6.3f}{m['coverage']:8.3f}{m['precision']:8.3f}{m['wrongTool']:7d}"
              f"{m['falseRoute']:8d}{m['falseRouteRate']:8.3%}{m['falseRouteUpper']:8.3%}"
              f"{m['authoredFalseRouteRate']:10.3%}{m['realFalseRouteRate']:8.3%}  {per}")


def print_families(title, summary):
    print(f"\n{title} at threshold {summary['threshold']}")
    print(f"  {'family':10s}{'pos':>5s}{'cover':>8s}{'prec':>8s}{'falseR':>8s}{'rate':>8s}{'up95':>8s}")
    for family, entry in summary["families"].items():
        print(f"  {family:10s}{entry['positives']:5d}{entry['coverage']:8.3f}{entry['precision']:8.3f}"
              f"{entry['falseRoute']:8d}{entry['falseRouteRate']:8.3%}{entry['falseRouteUpper']:8.3%}")
    m = summary["moduleFamilies"]
    print(f"  {'MODULES':10s}{m['positives']:5d}{m['coverage']:8.3f}{m['precision']:8.3f}"
          f"{m['falseRoute']:8d}{m['falseRouteRate']:8.3%}{m['falseRouteUpper']:8.3%}"
          f"  authored {m['authoredFalseRouteRate']:.3%} (n={m['authoredOthers']}, up95 "
          f"{m['authoredFalseRouteUpper']:.3%}), real {m['realFalseRouteRate']:.3%} (n={m['realOthers']})")


def check_gates(gates, summary):
    failures = []
    metrics = gates.get("router", {}).get("metrics", {})
    values = {"moduleFamilies.falseRouteRate": summary["moduleFamilies"]["falseRouteRate"],
              "moduleFamilies.precision": summary["moduleFamilies"]["precision"],
              "moduleFamilies.coverage": summary["moduleFamilies"]["coverage"]}
    for family, entry in summary["families"].items():
        values[f"{family}.falseRouteRate"] = entry["falseRouteRate"]
        values[f"{family}.precision"] = entry["precision"]
        values[f"{family}.coverage"] = entry["coverage"]
    for name, bound in metrics.items():
        if name not in values:
            failures.append(f"{name}: no such metric")
            continue
        if "max" in bound and values[name] > bound["max"]:
            failures.append(f"{name}: {values[name]:.4f} above {bound['max']}")
        if "min" in bound and values[name] < bound["min"]:
            failures.append(f"{name}: {values[name]:.4f} below {bound['min']}")
    return failures


ROUTE_TOOL = {"memory_save": "memory.remember", "memory_recall": "memory.recall",
              "reminder_set": "memory.remind", "memory_forget": "memory.forget"}


def load_traffic(path):
    cases = []
    for line in pathlib.Path(path).read_text().splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and parts[0]:
            cases.append({"text": parts[-1], "lang": parts[1] if len(parts) > 2 else "es", "role": "owner",
                          "expected": {ROUTE_TOOL[parts[0]]} if parts[0] in ROUTE_TOOL else set(),
                          "optional": ["app.show_camera"] if parts[0] == "camera" else [],
                          "label": parts[0], "stratum": "real", "set": pathlib.Path(path).stem})
    return cases


def traffic_share(cases, decisions, threshold):
    routed = correct = positives = false_actions = negatives = 0
    for case, (tool, confidence) in zip(cases, decisions):
        if tool is not None and confidence < threshold:
            tool = None
        routed += tool is not None
        if case["expected"]:
            positives += 1
            correct += tool in case["expected"]
        else:
            negatives += 1
            false_actions += tool is not None and tool not in case["optional"]
    return {"turns": len(cases), "routedShare": rate(routed, len(cases)),
            "conversationShare": 1 - rate(routed, len(cases)),
            "memoryCoverage": rate(correct, positives), "falseActionRate": rate(false_actions, negatives)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--traffic", nargs="*", default=[])
    parser.add_argument("--decider", required=True)
    parser.add_argument("--gates", required=True)
    parser.add_argument("--select", nargs="+", required=True)
    parser.add_argument("--select-negatives")
    parser.add_argument("--sealed")
    parser.add_argument("--final", action="store_true")
    parser.add_argument("--latency", type=int, default=200)
    parser.add_argument("--report")
    args = parser.parse_args()

    gates = json.loads(pathlib.Path(args.gates).read_text())
    ceiling = gates["sealed"]["falseRouteMax"]
    try:
        decider = Decider(args.decider)
    except OSError as error:
        print(f"decider-eval: cannot start the decider: {error}")
        return SKIP

    selection = load_cases(args.select)
    if args.select_negatives:
        selection += load_negatives(args.select_negatives)
    report = {"decider": args.decider, "selection": len(selection)}
    try:
        chosen_decisions = decider.decide_all(selection)
        print_sweep("selection sweep (cases, holdout, real negatives; chooses the operating point)",
                    selection, chosen_decisions)
        threshold = choose_threshold(selection, chosen_decisions, ceiling)
        report["threshold"] = threshold
        if threshold is None:
            print(f"\nno threshold keeps the false-route rate at or below {ceiling:.2%} on the selection set")
            report["selectionPassed"] = False
        else:
            print(f"\noperating point chosen on the selection set: confidence >= {threshold} "
                  f"(lowest threshold with the pooled, the per-family and the authored near-miss false-route rates all <= {ceiling:.2%})")
            report["selectionPassed"] = True
            report["selection"] = summarise(selection, chosen_decisions, threshold)
            print_families("selection", report["selection"])
        for path in args.traffic:
            traffic = load_traffic(path)
            share = traffic_share(traffic, decider.decide_all(traffic), threshold or 0.0)
            report.setdefault("traffic", {})[pathlib.Path(path).name] = share
            print(f"\nreal traffic {pathlib.Path(path).name} at confidence >= {threshold}: {share['turns']} turns, "
                  f"{share['routedShare']:.1%} reach a tool, {share['conversationShare']:.1%} stay conversation, "
                  f"memory coverage {share['memoryCoverage']:.1%}, false action {share['falseActionRate']:.1%}")
        sample = [c for c in selection if c["stratum"] == "authored"][:args.latency]
        if sample:
            times = decider.latencies(sample)
            report["latencyMs"] = {"p50": statistics.median(times),
                                   "p95": sorted(times)[max(0, int(len(times) * 0.95) - 1)], "n": len(times)}
            print(f"\ndecision latency over {len(times)} sequential requests: "
                  f"p50 {report['latencyMs']['p50']:.2f} ms, p95 {report['latencyMs']['p95']:.2f} ms")
        failures = []
        if args.final:
            sealed_path = pathlib.Path(args.sealed)
            actual = hashlib.sha256(sealed_path.read_bytes()).hexdigest()
            if actual != gates["sealed"]["sha256"]:
                print(f"the sealed set changed: {actual}")
                return 1
            sealed = load_cases([sealed_path])
            decisions = decider.decide_all(sealed)
            print("\nFINAL MEASUREMENT on the sealed set (sha256 " + actual + ")")
            print_sweep("sealed sweep, information only: the operating point is NOT chosen from it",
                        sealed, decisions)
            if threshold is not None:
                final = summarise(sealed, decisions, threshold)
                report["sealed"] = final
                print_families("sealed", final)
                failures = check_gates(gates, final)
                for name, value in rates_of(final).items():
                    if value > ceiling:
                        failures.append(f"sealed {name} false-route {value:.3%} above {ceiling:.2%}")
            else:
                failures.append("no operating point meets the ceiling on the selection set")
    finally:
        decider.close()
    report["offeredViolations"] = decider.violations
    if decider.violations:
        print(f"\n{decider.violations} answers named a tool that was not offered and were counted as none")
    report["failures"] = failures
    if args.report:
        pathlib.Path(args.report).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print()
    if failures:
        print("GATE FAILED")
        for failure in failures:
            print("  " + failure)
        return 1
    print("GATE PASSED" if args.final else "SELECTION DONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
