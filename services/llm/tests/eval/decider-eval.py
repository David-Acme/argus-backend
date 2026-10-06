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
ACTS = (0.5, 0.6, 0.7, 0.8, 0.85, 0.88, 0.9, 0.91, 0.92, 0.93, 0.94, 0.95, 0.96, 0.97, 0.98, 0.99, 0.995)
ASKS = (0.3, 0.5, 0.6, 0.7, 0.8, 0.85, 0.9, 0.93, 0.95)
MARGINS = (0.0, 0.05, 0.1, 0.2, 0.3, 0.5)
DEFAULT_WRONG_ACT = 0.001
DEFAULT_ASK_CLEAR = 0.10
DEFAULT_WRONG_TOOL = 0.01


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


def rate(hits, total):
    return hits / total if total else 0.0


def expected_of(record):
    expect = record.get("expect", {})
    ambiguous = expect.get("ambiguous")
    if ambiguous:
        return "ambiguous", set(ambiguous["tools"]), []
    calls = [call["tool"] for call in expect.get("calls") or []]
    if calls:
        return "positive", set(calls), []
    if record.get("role", "owner") != "owner":
        return None, set(), []
    for key, field in (("confirm", "tool"), ("inactive", "attempted"), ("offerAccept", "attempted")):
        entry = expect.get(key)
        if entry and entry.get(field):
            return "positive", {entry[field]}, []
    if "offerDecline" in expect:
        return None, set(), []
    if record.get("route") == "camera":
        return "negative", set(), ["app.show_camera"]
    return "negative", set(), []


def describe(case):
    wanted = case["expected"]
    case["families"] = {FAMILY_OF[t] for t in wanted if t in FAMILY_OF}
    case["authored"] = case["stratum"] != "real"
    return case


def load_cases(paths):
    cases, seen = [], set()
    for path in paths:
        for line in pathlib.Path(path).read_text().splitlines():
            if not line.strip():
                continue
            record = json.loads(line)
            kind, wanted, optional = expected_of(record)
            if kind is None:
                continue
            text = record["script"][0]
            key = (text, kind, tuple(sorted(wanted)))
            if key in seen:
                continue
            seen.add(key)
            cases.append(describe({
                "text": text, "lang": record["lang"], "role": record.get("role", "owner"),
                "kind": kind, "expected": wanted, "optional": optional,
                "variant": record.get("variant", "neutral"),
                "stratum": "real" if record.get("variant") == "real" else "authored",
                "set": pathlib.Path(path).stem}))
    return cases


def load_negatives(path):
    return [describe({"text": line.strip(), "lang": "es", "role": "owner", "kind": "negative",
                      "expected": set(), "optional": [], "variant": "real", "stratum": "real",
                      "set": pathlib.Path(path).stem})
            for line in pathlib.Path(path).read_text().splitlines() if line.strip()]


class Decider:
    def __init__(self, command):
        self.command = command
        self.process = None
        self.violations = 0

    def start(self):
        if self.process is None:
            self.process = subprocess.Popen(shlex.split(self.command), stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE, text=True, bufsize=1)

    def request(self, seq, case):
        return json.dumps({"seq": seq, "text": case["text"], "lang": case["lang"],
                           "role": case["role"], "tools": offered(case["role"])},
                          ensure_ascii=False)

    def decide_all(self, cases):
        self.start()

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
            runner = reply.get("runnerUp") or {}
            answers[reply["seq"]] = (reply.get("tool"), float(reply.get("confidence", 0.0)),
                                     runner.get("tool"), float(runner.get("confidence", 0.0)))
        writer.join()
        decisions = []
        for seq, case in enumerate(cases):
            tool, confidence, runner_tool, runner_confidence = answers[seq]
            allowed = offered(case["role"])
            if tool is not None and tool not in allowed:
                self.violations += 1
                tool, confidence = None, 0.0
            if runner_tool is not None and runner_tool not in allowed:
                runner_tool, runner_confidence = None, 0.0
            decisions.append((tool, confidence, runner_tool, runner_confidence))
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
        if self.process is not None:
            self.process.stdin.close()
            self.process.wait(timeout=60)


def outcome(decision, policy):
    tool, confidence, runner_tool, runner_confidence = decision
    act, ask, margin = policy
    if tool is None:
        return "none", ()
    gap = confidence - (runner_confidence if runner_tool else 0.0)
    if confidence >= act and gap >= margin:
        return "act", (tool,)
    if confidence >= ask:
        if runner_tool and runner_confidence >= ask:
            return "ask", (tool, runner_tool)
        return "ask", (tool,)
    return "none", ()


def blank():
    return {"positives": 0, "actCorrect": 0, "askCorrect": 0, "askAny": 0, "wrongTool": 0, "routed": 0,
            "others": 0, "wrongAct": 0, "askOther": 0, "authoredOthers": 0, "authoredWrongAct": 0,
            "ambiguous": 0, "ambiguousAct": 0, "ambiguousAskCorrect": 0, "ambiguousAsk": 0}


def tally(cases, decisions, policy, scopes):
    counts = {name: blank() for name in scopes}
    for case, decision in zip(cases, decisions):
        result, options = outcome(decision, policy)
        kind = case["kind"]
        for name, (members, select) in scopes.items():
            if select is not None and not select(case):
                continue
            entry = counts[name]
            wanted_here = case["families"] & members
            if kind == "ambiguous":
                if not (case["expected"] and {FAMILY_OF[t] for t in case["expected"] if t in FAMILY_OF} & members):
                    continue
                entry["ambiguous"] += 1
                if result == "act":
                    entry["ambiguousAct"] += 1
                elif result == "ask":
                    entry["ambiguousAsk"] += 1
                    entry["ambiguousAskCorrect"] += bool(set(options) & case["expected"])
                continue
            acted_here = result == "act" and FAMILY_OF.get(options[0]) in members
            if result == "act" and FAMILY_OF.get(options[0]) in members:
                entry["routed"] += 1
            if wanted_here and kind == "positive":
                entry["positives"] += 1
                if result == "act":
                    if options[0] in case["expected"]:
                        entry["actCorrect"] += 1
                    else:
                        entry["wrongTool"] += 1
                elif result == "ask":
                    entry["askAny"] += 1
                    entry["askCorrect"] += bool(set(options) & case["expected"])
            else:
                entry["others"] += 1
                entry["authoredOthers"] += case["authored"]
                if acted_here and options[0] not in case["optional"]:
                    entry["wrongAct"] += 1
                    entry["authoredWrongAct"] += case["authored"]
                if result == "ask" and FAMILY_OF.get(options[0]) in members:
                    entry["askOther"] += 1
    return counts


def scopes_of(select=None):
    out = {"moduleFamilies": (set(MODULE_FAMILIES), select)}
    for family in MODULE_FAMILIES:
        out[family] = ({family}, select)
    return out


def digest(entry):
    positives = entry["positives"]
    return {
        "positives": positives,
        "coverage": rate(entry["actCorrect"] + entry["askCorrect"], positives),
        "actCoverage": rate(entry["actCorrect"], positives),
        "precision": rate(entry["actCorrect"], entry["routed"]),
        "askRateClear": rate(entry["askAny"], positives),
        "wrongToolRate": rate(entry["wrongTool"], positives),
        "wrongAct": entry["wrongAct"], "others": entry["others"],
        "wrongActRate": rate(entry["wrongAct"], entry["others"]),
        "wrongActUpper": wilson(entry["wrongAct"], entry["others"])[1],
        "authoredWrongActRate": rate(entry["authoredWrongAct"], entry["authoredOthers"]),
        "authoredWrongActUpper": wilson(entry["authoredWrongAct"], entry["authoredOthers"])[1],
        "authoredOthers": entry["authoredOthers"],
        "askRateOther": rate(entry["askOther"], entry["others"]),
        "ambiguous": entry["ambiguous"],
        "ambiguousAskRate": rate(entry["ambiguousAskCorrect"], entry["ambiguous"]),
        "ambiguousActRate": rate(entry["ambiguousAct"], entry["ambiguous"]),
    }


def summarise(cases, decisions, policy):
    counts = tally(cases, decisions, policy, scopes_of())
    out = {"policy": {"act": policy[0], "ask": policy[1], "margin": policy[2]}, "cases": len(cases),
           "moduleFamilies": digest(counts["moduleFamilies"]),
           "families": {f: digest(counts[f]) for f in MODULE_FAMILIES}, "variants": {}}
    for variant in sorted({c["variant"] for c in cases}):
        part = tally(cases, decisions, policy, {"moduleFamilies": (set(MODULE_FAMILIES),
                                                                   lambda c, v=variant: c["variant"] == v)})
        out["variants"][variant] = digest(part["moduleFamilies"])
    return out


def feasible(summary, limits):
    rows = [summary["moduleFamilies"]] + [summary["families"][f] for f in MODULE_FAMILIES]
    if any(r["wrongActRate"] > limits["wrongAct"] or r["authoredWrongActRate"] > limits["wrongAct"] for r in rows):
        return False
    pooled = summary["moduleFamilies"]
    return pooled["askRateClear"] <= limits["askClear"] and pooled["wrongToolRate"] <= limits["wrongTool"]


def policies():
    for act in ACTS:
        for ask in (a for a in ASKS if a <= act):
            for margin in MARGINS:
                yield (act, ask, margin)


def act_only_policies():
    for act in ACTS:
        yield (act, act, 0.0)


def choose_policy(cases, decisions, limits):
    best, best_key = None, None
    for policy in policies():
        summary = summarise_pooled(cases, decisions, policy, limits)
        if summary is None:
            continue
        key = (summary["coverage"], -summary["askRateClear"], -summary["askRateOther"], -policy[0])
        if best_key is None or key > best_key:
            best, best_key = policy, key
    return best


def summarise_pooled(cases, decisions, policy, limits):
    counts = tally(cases, decisions, policy, scopes_of())
    rows = {name: digest(entry) for name, entry in counts.items()}
    if any(r["wrongActRate"] > limits["wrongAct"] or r["authoredWrongActRate"] > limits["wrongAct"]
           for r in rows.values()):
        return None
    pooled = rows["moduleFamilies"]
    if pooled["askRateClear"] > limits["askClear"] or pooled["wrongToolRate"] > limits["wrongTool"]:
        return None
    return pooled


def print_sweep(title, cases, decisions):
    print(f"\n{title}: {len(cases)} cases (act only: no ASK band)")
    print(f"  {'act':>6s}{'cover':>8s}{'prec':>8s}{'wrongT':>8s}{'wrongA':>8s}{'rate':>8s}{'up95':>8s}"
          f"{'authored':>10s}  per-family wrong ACT")
    for policy in act_only_policies():
        s = summarise(cases, decisions, policy)
        m = s["moduleFamilies"]
        per = " ".join(f"{f[:4]}={s['families'][f]['wrongActRate']:.2%}" for f in MODULE_FAMILIES)
        print(f"  {policy[0]:6.3f}{m['coverage']:8.3f}{m['precision']:8.3f}{m['wrongToolRate']:8.2%}"
              f"{m['wrongAct']:8d}{m['wrongActRate']:8.3%}{m['wrongActUpper']:8.3%}"
              f"{m['authoredWrongActRate']:10.3%}  {per}")


def print_families(title, summary):
    p = summary["policy"]
    print(f"\n{title} at ACT >= {p['act']} / ASK >= {p['ask']} / margin {p['margin']}")
    print(f"  {'family':10s}{'pos':>5s}{'cover':>8s}{'act':>8s}{'prec':>8s}{'askClr':>8s}{'wrongA':>8s}{'rate':>8s}"
          f"{'up95':>8s}{'amb':>5s}{'ambAsk':>8s}")
    for family, e in summary["families"].items():
        print(f"  {family:10s}{e['positives']:5d}{e['coverage']:8.3f}{e['actCoverage']:8.3f}{e['precision']:8.3f}"
              f"{e['askRateClear']:8.3f}{e['wrongAct']:8d}{e['wrongActRate']:8.3%}{e['wrongActUpper']:8.3%}"
              f"{e['ambiguous']:5d}{e['ambiguousAskRate']:8.3f}")
    m = summary["moduleFamilies"]
    print(f"  {'MODULES':10s}{m['positives']:5d}{m['coverage']:8.3f}{m['actCoverage']:8.3f}{m['precision']:8.3f}"
          f"{m['askRateClear']:8.3f}{m['wrongAct']:8d}{m['wrongActRate']:8.3%}{m['wrongActUpper']:8.3%}"
          f"{m['ambiguous']:5d}{m['ambiguousAskRate']:8.3f}"
          f"  near-miss {m['authoredWrongActRate']:.3%} (n={m['authoredOthers']}, up95 {m['authoredWrongActUpper']:.3%})"
          f", ask on others {m['askRateOther']:.3%}, wrong tool {m['wrongToolRate']:.2%}")
    for variant, e in summary["variants"].items():
        print(f"    variant {variant:8s} pos {e['positives']:4d} cover {e['coverage']:.3f} act {e['actCoverage']:.3f} "
              f"askClear {e['askRateClear']:.3f} wrong ACT {e['wrongActRate']:.3%} ({e['wrongAct']})")


def limits_of(gates):
    section = gates.get("decider", {})
    return {"wrongAct": section.get("wrongActMax", gates.get("sealed", {}).get("falseRouteMax", DEFAULT_WRONG_ACT)),
            "askClear": section.get("askRateClearMax", DEFAULT_ASK_CLEAR),
            "wrongTool": section.get("wrongToolActMax", DEFAULT_WRONG_TOOL)}


def check_gates(gates, summary, limits, label):
    failures = []
    rows = {"moduleFamilies": summary["moduleFamilies"], **summary["families"]}
    for name, row in rows.items():
        for key in ("wrongActRate", "authoredWrongActRate"):
            if row[key] > limits["wrongAct"]:
                failures.append(f"{label} {name}.{key}: {row[key]:.4%} above {limits['wrongAct']:.2%}")
    pooled = summary["moduleFamilies"]
    if pooled["askRateClear"] > limits["askClear"]:
        failures.append(f"{label} moduleFamilies.askRateClear: {pooled['askRateClear']:.4f} above {limits['askClear']}")
    if pooled["wrongToolRate"] > limits["wrongTool"]:
        failures.append(f"{label} moduleFamilies.wrongToolRate: {pooled['wrongToolRate']:.4f} above {limits['wrongTool']}")
    values = {f"{name}.{key}": value for name, row in rows.items() for key, value in row.items()}
    metrics = {**gates.get("router", {}).get("metrics", {}), **gates.get("decider", {}).get("metrics", {})}
    for name, bound in metrics.items():
        if name not in values:
            failures.append(f"{name}: no such metric")
            continue
        if "max" in bound and values[name] > bound["max"]:
            failures.append(f"{label} {name}: {values[name]:.4f} above {bound['max']}")
        if "min" in bound and values[name] < bound["min"]:
            failures.append(f"{label} {name}: {values[name]:.4f} below {bound['min']}")
    return failures


ROUTE_TOOL = {"memory_save": "memory.remember", "memory_recall": "memory.recall",
              "reminder_set": "memory.remind", "memory_forget": "memory.forget"}


def load_traffic(path):
    cases = []
    for line in pathlib.Path(path).read_text().splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and parts[0]:
            cases.append(describe({
                "text": parts[-1], "lang": parts[1] if len(parts) > 2 else "es", "role": "owner",
                "kind": "positive" if parts[0] in ROUTE_TOOL else "negative",
                "expected": {ROUTE_TOOL[parts[0]]} if parts[0] in ROUTE_TOOL else set(),
                "optional": ["app.show_camera"] if parts[0] == "camera" else [],
                "variant": "real", "stratum": "real", "set": pathlib.Path(path).stem}))
    return cases


def traffic_share(cases, decisions, policy):
    acted = asked = correct = positives = false_actions = negatives = 0
    for case, decision in zip(cases, decisions):
        result, options = outcome(decision, policy)
        acted += result == "act"
        asked += result == "ask"
        if case["kind"] == "positive":
            positives += 1
            correct += (result == "act" and options[0] in case["expected"]) or \
                       (result == "ask" and bool(set(options) & case["expected"]))
        else:
            negatives += 1
            false_actions += result == "act" and options[0] not in case["optional"]
    total = len(cases)
    return {"turns": total, "actShare": rate(acted, total), "askShare": rate(asked, total),
            "conversationShare": 1 - rate(acted + asked, total),
            "memoryCoverage": rate(correct, positives), "falseActionRate": rate(false_actions, negatives)}


def error_rows(cases, decisions, policy):
    rows = []
    for case, decision in zip(cases, decisions):
        result, options = outcome(decision, policy)
        wanted = case["expected"]
        if case["kind"] == "positive":
            good = (result == "act" and options[0] in wanted) or (result == "ask" and bool(set(options) & wanted))
            category = "right" if good else ("wrong-act" if result == "act" else ("wrong-ask" if result == "ask" else "miss"))
        elif case["kind"] == "ambiguous":
            category = "wrong-act" if result == "act" else ("right" if result == "ask" and set(options) & wanted
                                                            else ("wrong-ask" if result == "ask" else "miss"))
        else:
            category = "wrong-act" if result == "act" and options[0] not in case["optional"] else \
                ("spurious-ask" if result == "ask" else "right")
        if category != "right":
            rows.append({"category": category, "kind": case["kind"], "variant": case["variant"], "set": case["set"],
                         "text": case["text"], "expected": sorted(wanted), "outcome": result, "options": list(options),
                         "tool": decision[0], "confidence": decision[1], "runnerUp": decision[2],
                         "runnerConfidence": decision[3]})
    return rows


def cached_decisions(decider, cases, path):
    if not path:
        return decider.decide_all(cases)
    cache = pathlib.Path(path)
    keys = [hashlib.sha1((c["text"] + "\0" + c["role"]).encode()).hexdigest() for c in cases]
    if cache.exists():
        stored = json.loads(cache.read_text())
        if all(key in stored for key in keys):
            return [tuple(stored[key]) for key in keys]
    decisions = decider.decide_all(cases)
    cache.write_text(json.dumps({key: list(d) for key, d in zip(keys, decisions)}))
    return decisions


def read_sealed(path, section, gates):
    sealed_path = pathlib.Path(path)
    actual = hashlib.sha256(sealed_path.read_bytes()).hexdigest()
    if actual != gates[section]["sha256"]:
        print(f"the {section} set changed: {actual}")
        return None, actual
    return load_cases([sealed_path]), actual


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--decider", required=True)
    parser.add_argument("--gates", required=True)
    parser.add_argument("--select", nargs="+", required=True)
    parser.add_argument("--select-negatives")
    parser.add_argument("--traffic", nargs="*", default=[])
    parser.add_argument("--sealed")
    parser.add_argument("--sealed2")
    parser.add_argument("--final", action="store_true")
    parser.add_argument("--act-only", action="store_true")
    parser.add_argument("--latency", type=int, default=200)
    parser.add_argument("--report")
    parser.add_argument("--errors")
    parser.add_argument("--cache")
    args = parser.parse_args()

    gates = json.loads(pathlib.Path(args.gates).read_text())
    limits = limits_of(gates)
    decider = Decider(args.decider)

    selection = load_cases(args.select)
    if args.select_negatives:
        selection += load_negatives(args.select_negatives)
    report = {"decider": args.decider, "selectionCases": len(selection), "limits": limits}
    failures = []
    try:
        try:
            chosen_decisions = cached_decisions(decider, selection, args.cache)
        except OSError as error:
            print(f"decider-eval: cannot start the decider: {error}")
            return SKIP
        print_sweep("selection sweep (cases, holdout, real negatives; chooses the operating point)",
                    selection, chosen_decisions)
        if args.act_only:
            options = [p for p in act_only_policies() if summarise_pooled(selection, chosen_decisions, p, limits)]
            policy = max(options, key=lambda p: summarise_pooled(selection, chosen_decisions, p, limits)["coverage"],
                         default=None)
        else:
            policy = choose_policy(selection, chosen_decisions, limits)
        report["policy"] = None if policy is None else {"act": policy[0], "ask": policy[1], "margin": policy[2]}
        if policy is None:
            print(f"\nno ACT / ASK policy keeps every wrong-ACT rate at or below {limits['wrongAct']:.2%} "
                  f"with at most {limits['askClear']:.0%} asks on clear commands on the selection set")
            report["selectionPassed"] = False
        else:
            print(f"\npolicy chosen on the selection set: ACT >= {policy[0]}, ASK >= {policy[1]}, margin {policy[2]} "
                  f"(maximum coverage with every wrong-ACT rate, pooled, per family and on the near-miss stratum, "
                  f"at or below {limits['wrongAct']:.2%} and at most {limits['askClear']:.0%} asks on clear commands)")
            report["selectionPassed"] = True
            report["selection"] = summarise(selection, chosen_decisions, policy)
            print_families("selection", report["selection"])
        if args.errors:
            rows = error_rows(selection, chosen_decisions, policy or (0.9, 0.5, 0.1))
            pathlib.Path(args.errors).write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows))
            print(f"\n{len(rows)} selection-set errors written to {args.errors}")
        for path in args.traffic:
            traffic = load_traffic(path)
            share = traffic_share(traffic, decider.decide_all(traffic), policy or (1.01, 1.01, 0.0))
            report.setdefault("traffic", {})[pathlib.Path(path).name] = share
            print(f"\nreal traffic {pathlib.Path(path).name}: {share['turns']} turns, {share['actShare']:.1%} ACT, "
                  f"{share['askShare']:.1%} ASK, {share['conversationShare']:.1%} conversation, "
                  f"memory coverage {share['memoryCoverage']:.1%}, false action {share['falseActionRate']:.1%}")
        sample = [c for c in selection if c["stratum"] == "authored"][:args.latency]
        if sample and decider.process is not None:
            decider.start()
            times = decider.latencies(sample)
            report["latencyMs"] = {"p50": statistics.median(times),
                                   "p95": sorted(times)[max(0, int(len(times) * 0.95) - 1)], "n": len(times)}
            print(f"\ndecision latency over {len(times)} sequential requests: "
                  f"p50 {report['latencyMs']['p50']:.2f} ms, p95 {report['latencyMs']['p95']:.2f} ms")
        if args.final:
            for section, path in (("sealed", args.sealed), ("sealed2", args.sealed2)):
                if not path:
                    continue
                sealed, actual = read_sealed(path, section, gates)
                if sealed is None:
                    return 1
                decisions = decider.decide_all(sealed)
                print(f"\nFINAL MEASUREMENT on {section} (sha256 {actual})")
                print_sweep(f"{section} sweep, information only: the policy is NOT chosen from it", sealed, decisions)
                if policy is None:
                    failures.append(f"{section}: no operating point meets the ceilings on the selection set")
                    continue
                final = summarise(sealed, decisions, policy)
                report[section] = final
                print_families(section, final)
                failures += check_gates(gates, final, limits, section)
    finally:
        decider.close()
    report["offeredViolations"] = decider.violations
    if decider.violations:
        print(f"\n{decider.violations} answers named a tool that was not offered and were counted as none")
    report["failures"] = failures
    if args.report:
        pathlib.Path(args.report).write_text(json.dumps(report, indent=2, sort_keys=True, default=list) + "\n")
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
