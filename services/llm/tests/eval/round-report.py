#!/usr/bin/env python3
import argparse
import json
import pathlib
import sys

FAMILY_COLUMNS = ("positives", "coverage", "actCoverage", "precision", "askRateClear", "wrongAct", "wrongActRate",
                  "wrongActUpper", "ambiguous", "ambiguousAskRate")
HEADER = ("positives", "coverage", "ACT", "precision", "ASK on clear", "wrong ACT", "rate", "upper 95%", "ambiguous",
          "ambiguous ASK")
PERF_ROWS = (("latencyP50Ms", "latency p50 (ms)"), ("latencyP95Ms", "latency p95 (ms)"), ("cpuMsPerCall", "CPU ms per call"),
             ("residentMb", "resident (MB)"), ("peakMb", "peak (MB)"), ("threads", "threads"),
             ("artifactMb", "artifact (MB)"), ("loadSeconds", "load (s)"))


BAR = "0.456"
GATES = pathlib.Path(__file__).resolve().parent / "gates.json"


def beside_text(gates):
    entries = json.loads(pathlib.Path(gates).read_text()).get("decider", {}).get("gateObeyingFastText", {})
    return "gate-obeying fastText " + ", ".join(
        f"{entry['coverage']} on the {name} set {entry['label']}" for name, entry in entries.items())


def load(path):
    path = pathlib.Path(path)
    return json.loads(path.read_text()) if path.exists() else None


def percent(value):
    return f"{value:.2%}"


def cell(name, value):
    if name in ("positives", "wrongAct", "ambiguous"):
        return str(int(value))
    if name in ("wrongActRate", "wrongActUpper"):
        return percent(value)
    return f"{value:.3f}"


def operating_point(report, ceiling):
    rows = report.get("priceOfCeiling", [])
    if ceiling is not None:
        for row in rows:
            if abs(row["ceiling"] - ceiling) < 1e-12:
                return row["summary"], row["ceiling"], row is rows[0]
        return None, ceiling, False
    for index, row in enumerate(rows):
        if row["policy"] is not None:
            return row["summary"], row["ceiling"], index == 0
    return None, None, False


def policy_line(summary):
    policy = summary["policy"]
    return (f"ACT >= {policy['act']}, ASK >= {policy['ask']}, margin {policy['margin']}, second signal >= "
            f"{policy['now']}, memory writes {'guarded' if policy['guardMemory'] else 'not guarded'}")


def table(title, rows):
    out = [f"### {title}", "", "| " + " | ".join(("", *HEADER)) + " |", "|" + "---|" * (len(HEADER) + 1)]
    for name, entry in rows:
        out.append("| " + " | ".join((name, *(cell(c, entry[c]) for c in FAMILY_COLUMNS))) + " |")
    return out + [""]


def decider_section(name, report, ceiling, baseline, beside):
    summary, used, at_gate = operating_point(report, ceiling)
    out = [f"## {name}", ""]
    if summary is None:
        out += ["No policy keeps every wrong-ACT rate at or below any ceiling that was searched.", ""]
        return out, None
    limits = report.get("limits", {})
    fit, gate = limits.get("fitWrongAct"), limits.get("wrongAct")
    if at_gate and fit is not None and gate is not None and fit < gate:
        label = f"the {fit:.2%} fit ceiling (the gate is {gate:.2%}; the margin is for the final read)"
    else:
        label = "the 0.1% gate" if at_gate else f"the {used:.2%} ceiling (NOT the gate: information)"
    pooled = summary["moduleFamilies"]
    out += [f"Operating point chosen on the selection set at {label}: {policy_line(summary)}.", "",
            f"Selection cases {summary['cases']}; coverage {pooled['coverage']:.3f} (bar {baseline}; {beside}), ACT coverage "
            f"{pooled['actCoverage']:.3f}, precision {pooled['precision']:.3f}, ASK on clear commands "
            f"{pooled['askRateClear']:.3f}, wrong ACT {pooled['wrongAct']} of {pooled['others']} "
            f"({percent(pooled['wrongActRate'])}, upper 95% {percent(pooled['wrongActUpper'])}, information), near-miss stratum "
            f"{percent(pooled['authoredWrongActRate'])} of {pooled['authoredOthers']} (upper 95% "
            f"{percent(pooled['authoredWrongActUpper'])}, information), wrong tool {percent(pooled['wrongToolRate'])}.", ""]
    latency = report.get("latencyMs")
    if latency:
        out += [f"Decision latency over {latency['n']} sequential requests: p50 {latency['p50']:.1f} ms, p95 "
                f"{latency['p95']:.1f} ms (the machine state is in the performance table, not here).", ""]
    rows = [(family, entry) for family, entry in summary["families"].items()]
    rows.append(("modules, pooled", pooled))
    rows.append(("memory", summary["memory"]))
    out += table("Per family", rows)
    out += table("Per variant", sorted(summary["variants"].items()))
    return out, pooled


def price_section(name, report):
    rows = report.get("priceOfCeiling", [])
    if not rows:
        return []
    out = [f"### Price of the wrong-ACT ceiling, {name}", "", "| ceiling | coverage | ACT | ASK on clear | precision |",
           "|---|---|---|---|---|"]
    for row in rows:
        if row["summary"] is None:
            out.append(f"| {percent(row['ceiling'])} | no policy | | | |")
            continue
        pooled = row["summary"]["moduleFamilies"]
        out.append(f"| {percent(row['ceiling'])} | {pooled['coverage']:.3f} | {pooled['actCoverage']:.3f} | "
                   f"{pooled['askRateClear']:.3f} | {pooled['precision']:.3f} |")
    return out + [""]


def binding_section(name, report):
    rows = report.get("binding")
    if not rows:
        return []
    out = [f"### Which constraint binds, {name}", "",
           "| relaxed | coverage | ACT | ASK on clear | precision | wrong ACT | near-miss | policy |", "|---|---|---|---|---|---|---|---|"]
    for row in rows:
        if row["summary"] is None:
            out.append(f"| {row['relaxed']} | no policy | | | | | | |")
            continue
        pooled, policy = row["summary"]["moduleFamilies"], row["policy"]
        out.append(f"| {row['relaxed']} | {pooled['coverage']:.3f} | {pooled['actCoverage']:.3f} | {pooled['askRateClear']:.3f} | "
                   f"{pooled['precision']:.3f} | {percent(pooled['wrongActRate'])} | {percent(pooled['authoredWrongActRate'])} | "
                   f"ACT >= {policy['act']}, ASK >= {policy['ask']}, margin {policy['margin']}, now >= {policy['now']} |")
    return out + ["", "Each row is the best policy with that one constraint lifted and the others kept; the first row is the gate. "
                  "The policy fit judges point rates against the ceilings; a Wilson bound is taken only at the final read.", ""]


def slices_section(name, report):
    slices = report.get("slices")
    if not slices:
        return []
    out = [f"### Slices, {name}", "", "| slice | cases | modules positives | coverage | precision | wrong ACT | memory positives | "
           "coverage | precision | wrong ACT |", "|---|---|---|---|---|---|---|---|---|---|"]
    for grouping, labels in slices.items():
        for label, row in labels.items():
            m, k = row["moduleFamilies"], row["memory"]
            out.append(f"| {grouping} / {label} | {row['cases']} | {m['positives']} | {m['coverage']:.3f} | "
                       f"{m['precision']:.3f} | {m['wrongAct']} | {k['positives']} | {k['coverage']:.3f} | "
                       f"{k['precision']:.3f} | {k['wrongAct']} |")
    return out + ["", "Each slice is scored at the reference policy of the round, over the same selection set; a slice is an "
                  "association with the data a case resembles, not the effect of removing that data.", ""]


def calibration_section(name, calibration):
    if not calibration:
        return []
    out = [f"### Calibration fitted on the selection set, {name}", "",
           "| output | pairs | before | temperature | Platt | isotonic | chosen |", "|---|---|---|---|---|---|---|"]
    for output, row in calibration["report"].items():
        ece = row["heldOutEce"]
        out.append(f"| {output} | {row['pairs']} | {ece['before']:.4f} | {ece['temperature']:.4f} | {ece['platt']:.4f} | "
                   f"{ece['isotonic']:.4f} | {row['chosen']} |")
    out += ["", "The numbers are the expected calibration error on the held-out half of the selection pairs.", ""]
    for output, row in calibration["report"].items():
        reliability = row.get("reliability")
        if not reliability:
            continue
        out += [f"#### Reliability of {output}", "", "| bin | n before | confidence before | accuracy before | n after | "
                "confidence after | accuracy after |", "|---|---|---|---|---|---|---|"]
        after = {r["low"]: r for r in reliability["after"]}
        for before in reliability["before"]:
            later = after.get(before["low"])
            cells = ("", "", "") if later is None else (later["count"], f"{later['confidence']:.3f}", f"{later['accuracy']:.3f}")
            out.append(f"| {before['low']:.1f}-{before['high']:.1f} | {before['count']} | {before['confidence']:.3f} | "
                       f"{before['accuracy']:.3f} | " + " | ".join(str(c) for c in cells) + " |")
        out.append("")
    return out + ["Bins are ten equal widths over all the pairs; the same pairs the fit saw, so the table shows the shape and "
                  "the held-out error above is the honest number.", ""]


def traffic_section(name, report):
    traffic = report.get("traffic")
    if not traffic:
        return []
    out = [f"### Real traffic, {name}", "", "| file | turns | ACT | ASK | conversation | false action |", "|---|---|---|---|---|---|"]
    for file, share in traffic.items():
        out.append(f"| {file} | {share['turns']} | {percent(share['actShare'])} | {percent(share['askShare'])} | "
                   f"{percent(share['conversationShare'])} | {percent(share['falseActionRate'])} |")
    return out + [""]


def performance_section(name, perf):
    out = [f"### Performance and consumption, {name}", ""]
    if not perf:
        return out + ["Not measured on an idle machine in this round.", ""]
    metrics = perf["metrics"]
    out += ["| metric | value |", "|---|---|"]
    out += [f"| {label} | {metrics[key]:.2f} |" for key, label in PERF_ROWS if key in metrics]
    out.append(f"| idle machine | {'yes' if metrics.get('idle') else 'NO: not reportable'} |")
    out.append("")
    out.append("Gates: " + ("all met." if not perf["failures"] else "FAILED: " + "; ".join(perf["failures"])))
    return out + [""]


def comparison(rounds, ceiling, baseline, beside):
    out = ["## Comparison", "", "| round | coverage | ACT | precision | ASK on clear | wrong ACT rate | near-miss | wrong tool | ceiling |",
           "|---|---|---|---|---|---|---|---|---|"]
    for name, report in rounds:
        summary, used, at_gate = operating_point(report, ceiling)
        if summary is None:
            out.append(f"| {name} | no policy | | | | | | | |")
            continue
        p = summary["moduleFamilies"]
        out.append(f"| {name} | {p['coverage']:.3f} | {p['actCoverage']:.3f} | {p['precision']:.3f} | {p['askRateClear']:.3f} | "
                   f"{percent(p['wrongActRate'])} | {percent(p['authoredWrongActRate'])} | {percent(p['wrongToolRate'])} | "
                   f"{percent(used)}{'' if at_gate else ' (information)'} |")
    return out + ["", f"The coverage bar is {baseline}, fastText families-v1.1 at the sealed point where it broke the wrong-ACT gate; "
                  f"beside it, {beside}. A pass is under the gate and above the bar.", ""]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--round", action="append", required=True, metavar="NAME=DIR")
    parser.add_argument("--ceiling", type=float)
    parser.add_argument("--baseline", default=BAR)
    parser.add_argument("--gates", default=str(GATES))
    args = parser.parse_args()
    rounds, sections = [], []
    for spec in args.round:
        name, _, directory = spec.partition("=")
        report = load(pathlib.Path(directory) / "calibrated.json") or load(pathlib.Path(directory) / "uncalibrated.json")
        if report is None:
            print(f"round-report: no calibrated.json or uncalibrated.json in {directory}", file=sys.stderr)
            return 1
        rounds.append((name, report))
        body, _ = decider_section(name, report, args.ceiling, args.baseline, beside_text(args.gates))
        sections += body
        sections += price_section(name, report)
        sections += binding_section(name, report)
        sections += slices_section(name, report)
        sections += calibration_section(name, load(pathlib.Path(directory) / "calibration.json"))
        sections += traffic_section(name, report)
        sections += performance_section(name, load(pathlib.Path(directory) / "perf.json"))
    lines = ["# Decider round report", ""]
    if len(rounds) > 1:
        lines += comparison(rounds, args.ceiling, args.baseline, beside_text(args.gates))
    print("\n".join(lines + sections))
    return 0


if __name__ == "__main__":
    sys.exit(main())
