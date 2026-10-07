#!/usr/bin/env python3
import argparse
import hashlib
import json
import math
import pathlib
import shlex
import statistics
import subprocess
import importlib.util
import sys
import threading
import time

SKIP = 77
_SPEC = importlib.util.spec_from_file_location("calibration", pathlib.Path(__file__).resolve().parent / "calibration.py")
calibration = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(calibration)
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
HERE = pathlib.Path(__file__).resolve().parent
VISIBILITY = json.loads((HERE / "tool-visibility.json").read_text())
READ_TOOLS = set(json.loads((HERE.parents[1] / "src/feature/llm/services/turn/tool-effects.json").read_text())["readOnly"])
LOW_RISK_WRITES = {"memory.remember", "memory.remind"}
NOWS = (0.0, 0.5, 0.7, 0.8, 0.9)
SEARCH_ACTS = tuple(round(0.50 + 0.01 * step, 2) for step in range(50)) + (0.995,)
GUARD_ASKS = (0.5, 0.7, 0.85)
GUARD_MARGINS = (0.0, 0.1, 0.3)
ACTS = (0.5, 0.6, 0.7, 0.8, 0.85, 0.88, 0.9, 0.91, 0.92, 0.93, 0.94, 0.95, 0.96, 0.97, 0.98, 0.99, 0.995)
ASKS = (0.3, 0.5, 0.6, 0.7, 0.8, 0.85, 0.9)
MARGINS = (0.0, 0.05, 0.1, 0.2, 0.3, 0.5)
RELAXATIONS = ("askClear", "nearMiss", "wrongActFamily", "wrongActPooled", "wrongTool", "thinStratum")
CEILINGS = (0.0025, 0.005, 0.01, 0.02)
CACHE_BATCH = 250
RANK_PRESERVING = ("temperature", "platt")
DEFAULT_WRONG_ACT = 0.001
DEFAULT_ASK_CLEAR = 0.10
DEFAULT_WRONG_TOOL = 0.01
DEFAULT_MIN_STRATUM = 300
FALLBACK_POLICY = (0.9, 0.9, 0.0)


def offered(role):
    if role not in VISIBILITY["roles"]:
        raise SystemExit(f"tool-visibility.json has no tools for the role {role}")
    return [tool for tool in VISIBILITY["roles"][role] if tool in FAMILY_OF]


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
            if not line.lstrip().startswith("{"):
                continue
            reply = json.loads(line)
            runner = reply.get("runnerUp") or {}
            now = reply.get("now")
            answers[reply["seq"]] = (reply.get("tool"), float(reply.get("confidence", 0.0)),
                                     runner.get("tool"), float(runner.get("confidence", 0.0)),
                                     None if now is None else float(now))
        writer.join()
        decisions = []
        for seq, case in enumerate(cases):
            tool, confidence, runner_tool, runner_confidence, now = answers[seq]
            allowed = offered(case["role"])
            if tool is not None and tool not in allowed:
                self.violations += 1
                tool, confidence = None, 0.0
            if runner_tool is not None and runner_tool not in allowed:
                runner_tool, runner_confidence = None, 0.0
            decisions.append((tool, confidence, runner_tool, runner_confidence, now))
        return decisions

    def latencies(self, cases):
        out = []
        for seq, case in enumerate(cases):
            started = time.perf_counter()
            self.process.stdin.write(self.request(seq, case) + "\n")
            self.process.stdin.flush()
            while True:
                line = self.process.stdout.readline()
                if not line:
                    raise SystemExit("decider closed its output during the latency sample")
                if line.lstrip().startswith("{"):
                    break
            out.append((time.perf_counter() - started) * 1000.0)
        return out

    def close(self):
        if self.process is not None:
            self.process.stdin.close()
            self.process.wait(timeout=60)


def outcome(decision, policy):
    tool, confidence, runner_tool, runner_confidence = decision[:4]
    now = decision[4] if len(decision) > 4 else None
    act, ask, margin = policy[:3]
    now_min = policy[3] if len(policy) > 3 else 0.0
    guard_memory = policy[4] if len(policy) > 4 else 1
    if tool is None:
        return "none", ()
    gap = confidence - (runner_confidence if runner_tool else 0.0)
    if confidence >= act and gap >= margin:
        guarded = tool not in READ_TOOLS and (guard_memory or tool not in LOW_RISK_WRITES)
        if now_min > 0.0 and guarded and (now is None or now < now_min):
            return "ask", (tool,)
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


def scopes_of(select=None, reported=()):
    out = {"moduleFamilies": (set(MODULE_FAMILIES), select)}
    for family in MODULE_FAMILIES:
        out[family] = ({family}, select)
    for family in reported:
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
        "authoredWrongAct": entry["authoredWrongAct"],
        "authoredWrongActRate": rate(entry["authoredWrongAct"], entry["authoredOthers"]),
        "authoredWrongActUpper": wilson(entry["authoredWrongAct"], entry["authoredOthers"])[1],
        "authoredOthers": entry["authoredOthers"],
        "askRateOther": rate(entry["askOther"], entry["others"]),
        "ambiguous": entry["ambiguous"],
        "ambiguousAskRate": rate(entry["ambiguousAskCorrect"], entry["ambiguous"]),
        "ambiguousActRate": rate(entry["ambiguousAct"], entry["ambiguous"]),
    }


def summarise(cases, decisions, policy):
    counts = tally(cases, decisions, policy, scopes_of(reported=("memory",)))
    out = {"policy": {"act": policy[0], "ask": policy[1], "margin": policy[2],
                      "now": policy[3] if len(policy) > 3 else 0.0,
                      "guardMemory": bool(policy[4]) if len(policy) > 4 else True}, "cases": len(cases),
           "moduleFamilies": digest(counts["moduleFamilies"]), "memory": digest(counts["memory"]),
           "families": {f: digest(counts[f]) for f in MODULE_FAMILIES}, "variants": {}}
    for variant in sorted({c["variant"] for c in cases}):
        part = tally(cases, decisions, policy, {"moduleFamilies": (set(MODULE_FAMILIES),
                                                                   lambda c, v=variant: c["variant"] == v)})
        out["variants"][variant] = digest(part["moduleFamilies"])
    return out


def policies(guard, scopes=(1,)):
    margins, nows = (GUARD_MARGINS, NOWS) if guard else (MARGINS, (0.0,))
    asks = GUARD_ASKS if guard else ASKS
    for act in SEARCH_ACTS:
        for ask in sorted({act, *(a for a in asks if a < act)}):
            for margin in margins:
                for now in nows:
                    for scope in (scopes if now > 0.0 else (1,)):
                        yield (act, ask, margin, now, scope)


def act_only_policies():
    for act in ACTS:
        yield (act, act, 0.0)


def rank_key(pooled, policy):
    return (pooled["coverage"], -pooled["askRateClear"], -pooled["askRateOther"], -policy[0])


def best_per_limit(cases, decisions, all_limits, scopes=(1,)):
    best = [{} for _ in all_limits]
    guard = any(len(d) > 4 and d[4] is not None for d in decisions)
    for policy in policies(guard, scopes):
        rows, pooled = pooled_rows(cases, decisions, policy)
        key = rank_key(pooled, policy)
        scope = policy[4] if len(policy) > 4 else 1
        for slot, limits in zip(best, all_limits):
            if limits.get("secondSignalOff") and policy[3] > 0.0:
                continue
            if within(rows, pooled, limits) and (scope not in slot or key > slot[scope][1]):
                slot[scope] = (policy, key)
    return best


def choose_policies(cases, decisions, limits, scopes=(1,)):
    return best_per_limit(cases, decisions, [limits], scopes)[0]


def choose_policy(cases, decisions, limits, scopes=(1,)):
    best = choose_policies(cases, decisions, limits, scopes)
    if not best:
        return None
    return max(best.values(), key=lambda item: item[1])[0]


def pooled_rows(cases, decisions, policy):
    counts = tally(cases, decisions, policy, scopes_of())
    rows = {name: digest(entry) for name, entry in counts.items()}
    return rows, rows["moduleFamilies"]


def violations(rows, pooled, limits):
    ceiling = limits["wrongAct"]
    found = set()
    for name, row in rows.items():
        if row["others"] < limits["minStratum"] or row["authoredOthers"] < limits["minStratum"]:
            found.add("thinStratum")
        if row["wrongActRate"] > ceiling:
            found.add("wrongActPooled" if name == "moduleFamilies" else "wrongActFamily")
        if row["authoredWrongActRate"] > ceiling:
            found.add("nearMiss")
    if pooled["askRateClear"] > limits["askClear"]:
        found.add("askClear")
    if pooled["wrongToolRate"] > limits["wrongTool"]:
        found.add("wrongTool")
    return found


def within(rows, pooled, limits):
    return not violations(rows, pooled, limits) - set(limits.get("relax", ()))


def summarise_pooled(cases, decisions, policy, limits):
    rows, pooled = pooled_rows(cases, decisions, policy)
    return pooled if within(rows, pooled, limits) else None


def price_table(cases, decisions, all_limits, found):
    rows = []
    print("\nprice of the wrong-ACT ceiling on the selection set (the gate is the first row; the others are "
          "information, never a pass)")
    print(f"  {'ceiling':>8s}{'cover':>8s}{'act':>8s}{'askClr':>8s}{'prec':>8s}  policy")
    for limits, best in zip(all_limits, found):
        policy = policy_of(best)
        if policy is None:
            print(f"  {limits['wrongAct']:8.2%}  no policy keeps every wrong-ACT rate at or below it")
            rows.append({"ceiling": limits["wrongAct"], "policy": None, "summary": None})
            continue
        summary = summarise(cases, decisions, policy)
        pooled = summary["moduleFamilies"]
        print(f"  {limits['wrongAct']:8.2%}{pooled['coverage']:8.3f}{pooled['actCoverage']:8.3f}"
              f"{pooled['askRateClear']:8.3f}{pooled['precision']:8.3f}  ACT >= {policy[0]} ASK >= {policy[1]} "
              f"margin {policy[2]}" + (f" now >= {policy[3]}" if len(policy) > 3 and policy[3] > 0.0 else ""))
        rows.append({"ceiling": limits["wrongAct"], "policy": summary["policy"], "summary": summary})
    return rows


def relaxed_limits(limits):
    return [dict(limits, wrongAct=ceiling) for ceiling in CEILINGS if ceiling > limits["wrongAct"]]


def binding_limits(limits):
    return [dict(limits, relax=(name,)) for name in RELAXATIONS] + [dict(limits, secondSignalOff=True)]


def binding_table(cases, decisions, found):
    names = ("nothing relaxed", *RELAXATIONS, "second signal off")
    print("\nwhich constraint binds: the best policy with one constraint relaxed at a time (the first row is the gate)")
    print(f"  {'relaxed':18s}{'cover':>8s}{'act':>8s}{'askClr':>8s}{'prec':>8s}{'wrongA':>8s}{'near-miss':>10s}  policy")
    rows = []
    for name, best in zip(names, found):
        policy = policy_of(best)
        if policy is None:
            print(f"  {name:18s}  no policy")
            rows.append({"relaxed": name, "policy": None, "summary": None})
            continue
        summary = summarise(cases, decisions, policy)
        pooled = summary["moduleFamilies"]
        print(f"  {name:18s}{pooled['coverage']:8.3f}{pooled['actCoverage']:8.3f}{pooled['askRateClear']:8.3f}"
              f"{pooled['precision']:8.3f}{pooled['wrongActRate']:8.3%}{pooled['authoredWrongActRate']:10.3%}  "
              f"ACT >= {policy[0]} ASK >= {policy[1]} margin {policy[2]} now >= {policy[3]}")
        rows.append({"relaxed": name, "policy": summary["policy"], "summary": summary})
    return rows


def error_free_needed(ceiling, z):
    return math.ceil(z * z * (1 - ceiling) / ceiling)


def certify(readings, ceiling, z):
    errors = sum(reading["authoredWrongAct"] for reading in readings.values())
    others = sum(reading["authoredOthers"] for reading in readings.values())
    upper = wilson(errors, others, z)[1]
    return {"sets": sorted(readings), "errors": errors, "others": others, "upper": upper, "ceiling": ceiling, "z": z,
            "errorFreeNeeded": error_free_needed(ceiling, z), "passed": others > 0 and upper <= ceiling}


def policy_of(best):
    return max(best.values(), key=lambda item: item[1])[0] if best else None


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
    print(f"\n{title} at ACT >= {p['act']} / ASK >= {p['ask']} / margin {p['margin']} / now >= {p['now']} (memory writes {'guarded' if p['guardMemory'] else 'not guarded'})")
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


def print_bar(gates, section, summary):
    section_gates = gates.get("decider", {})
    bar = section_gates.get("metrics", {}).get("moduleFamilies.coverage", {}).get("min")
    beside = section_gates.get("gateObeyingFastText", {}).get(section)
    if bar is None or beside is None:
        return
    coverage = summary["moduleFamilies"]["coverage"]
    print(f"  coverage {coverage:.3f} against the bar {bar} (fastText where it broke the wrong-ACT gate) and "
          f"{beside['coverage']} (gate-obeying fastText on the {section} set {beside['label']})")


def slice_rows(cases, decisions, policy, groupings):
    out = {}
    for grouping, labels in groupings.items():
        members = {}
        for index, case in enumerate(cases):
            label = labels.get(case["text"])
            if label is not None:
                members.setdefault(label, []).append(index)
        out[grouping] = {}
        for label, indexes in sorted(members.items()):
            summary = summarise([cases[i] for i in indexes], [decisions[i] for i in indexes], policy)
            out[grouping][label] = {"cases": len(indexes), "moduleFamilies": summary["moduleFamilies"],
                                    "memory": summary["memory"]}
    return out


def print_slices(slices, policy):
    print(f"\nslices at ACT >= {policy[0]} / ASK >= {policy[1]} / margin {policy[2]} (the reference policy)")
    print(f"  {'slice':34s}{'cases':>6s}  {'modules: pos':>12s}{'cover':>7s}{'prec':>7s}{'wrongA':>7s}"
          f"  {'memory: pos':>11s}{'cover':>7s}{'prec':>7s}{'wrongA':>7s}")
    for grouping, labels in slices.items():
        for label, row in labels.items():
            m, k = row["moduleFamilies"], row["memory"]
            print(f"  {grouping + ' / ' + label:34s}{row['cases']:6d}  {m['positives']:12d}{m['coverage']:7.3f}"
                  f"{m['precision']:7.3f}{m['wrongAct']:7d}  {k['positives']:11d}{k['coverage']:7.3f}"
                  f"{k['precision']:7.3f}{k['wrongAct']:7d}")


def print_reliability(name, reliability):
    for when in ("before", "after"):
        print(f"  reliability of {name}, {when} (all pairs, ten equal bins)")
        print(f"    {'bin':>10s}{'n':>7s}{'mean conf':>11s}{'accuracy':>10s}")
        for row in reliability[when]:
            print(f"    {row['low']:4.1f}-{row['high']:4.1f}{row['count']:7d}{row['confidence']:11.3f}{row['accuracy']:10.3f}")


def limits_of(gates):
    section = gates.get("decider", {})
    return {"wrongAct": section.get("wrongActMax", gates.get("sealed", {}).get("falseRouteMax", DEFAULT_WRONG_ACT)),
            "askClear": section.get("askRateClearMax", DEFAULT_ASK_CLEAR),
            "wrongTool": section.get("wrongToolActMax", DEFAULT_WRONG_TOOL),
            "minStratum": section.get("minStratum", DEFAULT_MIN_STRATUM)}


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


def certification(gates, report, limits):
    rule = gates.get("decider", {}).get("certification")
    if not rule:
        return []
    missing = [name for name in rule["pool"] if name not in report]
    if missing:
        return [f"certification needs the final read of {', '.join(rule['pool'])}; not read: {', '.join(missing)}"]
    if limits["wrongAct"] <= 0.0:
        return ["certification: a ceiling of zero cannot be certified by an upper bound"]
    result = certify({name: report[name]["moduleFamilies"] for name in rule["pool"]}, limits["wrongAct"], rule["z"])
    report["certification"] = result
    print(f"\nCERTIFICATION of the near-miss stratum, pooled over {', '.join(result['sets'])}: {result['errors']} wrong ACT "
          f"in {result['others']}, Wilson upper bound (z {result['z']}) {result['upper']:.4%} against the ceiling "
          f"{result['ceiling']:.2%}; error-free near-misses needed at this ceiling: {result['errorFreeNeeded']}")
    if result["passed"]:
        return []
    return [f"certification: the near-miss upper bound {result['upper']:.4%} is above {result['ceiling']:.2%} "
            f"({result['errors']} errors in {result['others']}; {result['errorFreeNeeded']} error-free needed)"]


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


def calibration_pairs(cases, decisions):
    confidence, now = [], []
    for case, decision in zip(cases, decisions):
        tool = decision[0]
        if tool is not None:
            confidence.append((decision[1], case["kind"] == "positive" and tool in case["expected"]))
            if len(decision) > 4 and decision[4] is not None:
                now.append((decision[4], case["kind"] == "positive"))
    return confidence, now


def fit_calibration(cases, decisions):
    confidence, now = calibration_pairs(cases, decisions)
    out = {"report": {}}
    for name, pairs in (("confidence", confidence), ("now", now)):
        if not pairs:
            continue
        train = [p for i, p in enumerate(pairs) if i % 2 == 0]
        held = [p for i, p in enumerate(pairs) if i % 2 == 1]
        rows = {"before": calibration.evaluate({"type": "identity"}, held)["ece"]}
        best_kind, best_ece = "identity", rows["before"]
        for kind in ("temperature", "platt", "isotonic"):
            model = calibration.fit(train, kind)
            rows[kind] = calibration.evaluate(model, held)["ece"]
            if kind in RANK_PRESERVING and rows[kind] < best_ece:
                best_kind, best_ece = kind, rows[kind]
        out[name] = calibration.fit(pairs, best_kind) if best_kind != "identity" else {"type": "identity"}
        after = [(calibration.apply(out[name], confidence), correct) for confidence, correct in pairs]
        out["report"][name] = {"pairs": len(pairs), "heldOutEce": rows, "chosen": best_kind,
                               "reliability": {"before": calibration.reliability(pairs),
                                               "after": calibration.reliability(after)}}
    return out


def apply_calibration(decisions, model):
    out = []
    for decision in decisions:
        tool, confidence, runner_tool, runner_confidence = decision[:4]
        now = decision[4] if len(decision) > 4 else None
        mapped = calibration.apply(model["confidence"], confidence) if "confidence" in model and tool else confidence
        mapped_runner = calibration.apply(model["confidence"], runner_confidence) \
            if "confidence" in model and runner_tool else runner_confidence
        mapped_now = calibration.apply(model["now"], now) if "now" in model and now is not None else now
        out.append((tool, mapped, runner_tool, mapped_runner, mapped_now))
    return out


def cache_key(case):
    return hashlib.sha1((case["text"] + "\0" + case["role"]).encode()).hexdigest()


def cached_decisions(decider, cases, path):
    if not path:
        return decider.decide_all(cases)
    cache = pathlib.Path(path)
    stored = json.loads(cache.read_text()) if cache.exists() else {}
    missing = [c for c in cases if cache_key(c) not in stored]
    for start in range(0, len(missing), CACHE_BATCH):
        batch = missing[start:start + CACHE_BATCH]
        for case, decision in zip(batch, decider.decide_all(batch)):
            stored[cache_key(case)] = list(decision)
        partial = cache.with_name(cache.name + ".part")
        partial.write_text(json.dumps(stored))
        partial.replace(cache)
    return [tuple(stored[cache_key(c)]) for c in cases]


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
    parser.add_argument("--chunk")
    parser.add_argument("--slices")
    parser.add_argument("--calibrate-out")
    parser.add_argument("--calibration")
    parser.add_argument("--guard-scope", choices=["both", "all", "low-risk-open"], default="both")
    args = parser.parse_args()

    gates = json.loads(pathlib.Path(args.gates).read_text())
    limits = limits_of(gates)
    decider = Decider(args.decider)

    selection = load_cases(args.select)
    if args.select_negatives:
        selection += load_negatives(args.select_negatives)
    if args.chunk:
        index, count = (int(part) for part in args.chunk.split("/"))
        if not 0 <= index < count:
            raise SystemExit(f"--chunk {args.chunk}: the index must be below the count")
        part = [c for i, c in enumerate(selection) if i % count == index]
        try:
            filled = cached_decisions(decider, part, args.cache)
        except OSError as error:
            print(f"decider-eval: cannot start the decider: {error}")
            return SKIP
        finally:
            decider.close()
        print(f"chunk {index}/{count}: {len(filled)} selection decisions are in the cache")
        return 0
    report = {"decider": args.decider, "selectionCases": len(selection), "limits": limits}
    failures = []
    found = []
    try:
        try:
            chosen_decisions = cached_decisions(decider, selection, args.cache)
        except OSError as error:
            print(f"decider-eval: cannot start the decider: {error}")
            return SKIP
        if args.calibrate_out:
            fitted = fit_calibration(selection, chosen_decisions)
            pathlib.Path(args.calibrate_out).write_text(json.dumps(fitted, indent=2, sort_keys=True) + "\n")
            for name, row in fitted["report"].items():
                print(f"\ncalibration of {name} on {row['pairs']} decisions, held-out expected calibration error: "
                      + ", ".join(f"{k} {v:.4f}" for k, v in row["heldOutEce"].items()) + f" -> {row['chosen']}")
                print_reliability(name, row["reliability"])
        if args.calibration:
            chosen_decisions = apply_calibration(chosen_decisions, json.loads(pathlib.Path(args.calibration).read_text()))
            print(f"\nconfidences calibrated by {args.calibration}")
        print_sweep("selection sweep (cases, holdout, real negatives; chooses the operating point)",
                    selection, chosen_decisions)
        if args.act_only:
            options = [p for p in act_only_policies() if summarise_pooled(selection, chosen_decisions, p, limits)]
            policy = max(options, key=lambda p: summarise_pooled(selection, chosen_decisions, p, limits)["coverage"],
                         default=None)
        else:
            scopes = {"both": (1, 0), "all": (1,), "low-risk-open": (0,)}[args.guard_scope]
            relaxed = relaxed_limits(limits)
            priced = [limits] + relaxed
            searched = best_per_limit(selection, chosen_decisions, priced + binding_limits(limits), scopes)
            found = searched[:len(priced)]
            per_scope = found[0]
            policy = policy_of(per_scope)
            report["priceOfCeiling"] = price_table(selection, chosen_decisions, priced, found)
            report["binding"] = binding_table(selection, chosen_decisions, [found[0]] + searched[len(priced):])
            if args.guard_scope == "both":
                for scope, name in ((1, "every write guarded"), (0, "memory writes not guarded")):
                    one = per_scope.get(scope, (None,))[0]
                    full = None if one is None else summarise(selection, chosen_decisions, one)
                    summary = None if full is None else full["moduleFamilies"]
                    print(f"\nbest policy with {name}: " + ("none" if one is None else
                          f"ACT >= {one[0]} ASK >= {one[1]} margin {one[2]} now >= {one[3] if len(one) > 3 else 0.0}, "
                          f"coverage {summary['coverage']:.3f}, ask on clear {summary['askRateClear']:.3f}; memory family: "
                          f"coverage {full['memory']['coverage']:.3f}, wrong ACT {full['memory']['wrongActRate']:.3%}"))
        report["policy"] = None if policy is None else {"act": policy[0], "ask": policy[1], "margin": policy[2],
                                                         "now": policy[3] if len(policy) > 3 else 0.0,
                                                         "guardMemory": bool(policy[4]) if len(policy) > 4 else True}
        if policy is None:
            print(f"\nno ACT / ASK policy keeps every wrong-ACT rate at or below {limits['wrongAct']:.2%} "
                  f"with at most {limits['askClear']:.0%} asks on clear commands on the selection set")
            report["selectionPassed"] = False
        else:
            print(f"\npolicy chosen on the selection set: ACT >= {policy[0]}, ASK >= {policy[1]}, margin {policy[2]}, "
                  f"second signal >= {policy[3] if len(policy) > 3 else 0.0} on write tools "
                  f"(maximum coverage with every wrong-ACT rate, pooled, per family and on the near-miss stratum, "
                  f"at or below {limits['wrongAct']:.2%} and at most {limits['askClear']:.0%} asks on clear commands)")
            report["selectionPassed"] = True
            report["selection"] = summarise(selection, chosen_decisions, policy)
            print_families("selection", report["selection"])
            print_bar(gates, "selection", report["selection"])
        for row in report.get("priceOfCeiling", []):
            if policy is None and row["policy"] is not None and row["ceiling"] > limits["wrongAct"]:
                print_families(f"selection at the {row['ceiling']:.2%} ceiling (NOT the gate: information)",
                               row["summary"])
                break
        if args.slices:
            reference = policy or next((p for p in map(policy_of, found) if p), None) or FALLBACK_POLICY
            report["slices"] = slice_rows(selection, chosen_decisions, reference,
                                          json.loads(pathlib.Path(args.slices).read_text()))
            print_slices(report["slices"], reference)
        if args.errors:
            rows = error_rows(selection, chosen_decisions, policy or FALLBACK_POLICY)
            pathlib.Path(args.errors).write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows))
            print(f"\n{len(rows)} selection-set errors written to {args.errors}")
        for path in args.traffic:
            traffic = load_traffic(path)
            heard = decider.decide_all(traffic)
            if args.calibration:
                heard = apply_calibration(heard, json.loads(pathlib.Path(args.calibration).read_text()))
            share = traffic_share(traffic, heard, policy or FALLBACK_POLICY)
            report.setdefault("traffic", {})[pathlib.Path(path).name] = share
            note = "" if policy else f" (no feasible policy: reported at ACT >= {FALLBACK_POLICY[0]})"
            print(f"\nreal traffic {pathlib.Path(path).name}{note}: {share['turns']} turns, {share['actShare']:.1%} ACT, "
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
                if args.calibration:
                    decisions = apply_calibration(decisions, json.loads(pathlib.Path(args.calibration).read_text()))
                print(f"\nFINAL MEASUREMENT on {section} (sha256 {actual})")
                print_sweep(f"{section} sweep, information only: the policy is NOT chosen from it", sealed, decisions)
                if policy is None:
                    failures.append(f"{section}: no operating point meets the ceilings on the selection set")
                    continue
                final = summarise(sealed, decisions, policy)
                report[section] = final
                print_families(section, final)
                print_bar(gates, section, final)
                failures += check_gates(gates, final, limits, section)
            if policy is not None:
                failures += certification(gates, report, limits)
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
