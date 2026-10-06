#!/usr/bin/env python3
import argparse
import json
import os
import pathlib
import shlex
import statistics
import subprocess
import sys
import time

SKIP = 77
CLOCK_TICKS = os.sysconf("SC_CLK_TCK")
SAMPLE_TEXTS = [
    ("es", "agéndame una reunión con Andrea el jueves a las tres de la tarde"),
    ("es", "qué tareas me quedan por hacer esta semana"),
    ("es", "recuérdame sacar la carne del congelador a las cinco"),
    ("es", "ayer tuve una reunión que no terminaba nunca"),
    ("es", "apaga el módulo de vigilancia por favor"),
    ("es", "eh crea una tarea de limpiar la canaleta del techo"),
    ("es", "ya pues dime qué proyectos tengo abiertos pe"),
    ("es", "la tarea de mi hijo me tuvo despierta hasta las once"),
    ("en", "what is on my calendar for friday afternoon"),
    ("en", "add a task to replace the smoke detector batteries"),
    ("en", "my phone calendar is full of spam"),
    ("en", "turn off the surveillance module please"),
    ("es", "pon la vigilancia en modo noche"),
    ("es", "muéstrame la cámara del garaje"),
    ("en", "tell me a joke about never ending projects"),
    ("es", "quiero borrar todo lo que guardó productividad"),
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


def descendants(pid):
    children = {}
    for entry in pathlib.Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
            parent = int(stat[stat.rindex(")") + 2:].split()[1])
        except (OSError, ValueError):
            continue
        children.setdefault(parent, []).append(int(entry.name))
    out, stack = [], [pid]
    while stack:
        current = stack.pop()
        out.append(current)
        stack.extend(children.get(current, []))
    return out


def status_kb(pid, key):
    try:
        for line in pathlib.Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith(key + ":"):
                return int(line.split()[1])
    except OSError:
        return 0
    return 0


def cpu_ticks(pid):
    try:
        stat = pathlib.Path(f"/proc/{pid}/stat").read_text()
        fields = stat[stat.rindex(")") + 2:].split()
        return int(fields[11]) + int(fields[12])
    except (OSError, ValueError):
        return 0


def tree_sum(pid, reader):
    return sum(reader(p) for p in descendants(pid))


def size_mb(paths):
    total = 0
    for raw in paths:
        path = pathlib.Path(raw)
        if path.is_dir():
            total += sum(f.stat().st_size for f in path.rglob("*") if f.is_file())
        elif path.exists():
            total += path.stat().st_size
    return total / (1024 * 1024)


def percentile(values, share):
    ordered = sorted(values)
    return ordered[max(0, int(len(ordered) * share) - 1)]


def exchange(process, seq, lang, text):
    request = {"seq": seq, "text": text, "lang": lang, "role": "owner",
               "tools": ["calendar.create_event", "calendar.list_events", "calendar.cancel_event", "task.create",
                         "task.list", "task.complete", "project.create", "project.list", "modules.list",
                         "modules.explain", "modules.enable", "modules.disable", "modules.open_purge_screen",
                         "reminder.list", "memory.remember", "memory.recall", "memory.remind", "memory.forget",
                         "app.open", "app.set_guard_mode", "app.show_camera"]}
    started = time.perf_counter()
    process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
    process.stdin.flush()
    while not process.stdout.readline().lstrip().startswith("{"):
        pass
    return (time.perf_counter() - started) * 1000.0


def measure(args):
    process = subprocess.Popen(shlex.split(args.decider), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               text=True, bufsize=1)
    try:
        started = time.perf_counter()
        exchange(process, 0, *SAMPLE_TEXTS[0])
        loadSeconds = time.perf_counter() - started
        for index in range(args.warmup):
            exchange(process, index, *SAMPLE_TEXTS[index % len(SAMPLE_TEXTS)])
        ticks_before = tree_sum(process.pid, cpu_ticks)
        threads_peak = tree_sum(process.pid, lambda p: status_kb(p, "Threads"))
        times = []
        for index in range(args.requests):
            lang, text = SAMPLE_TEXTS[index % len(SAMPLE_TEXTS)]
            times.append(exchange(process, index, lang, text))
            if index % 25 == 0:
                threads_peak = max(threads_peak, tree_sum(process.pid, lambda p: status_kb(p, "Threads")))
        ticks_after = tree_sum(process.pid, cpu_ticks)
        resident = tree_sum(process.pid, lambda p: status_kb(p, "VmRSS")) / 1024
        peak = tree_sum(process.pid, lambda p: status_kb(p, "VmHWM")) / 1024
    finally:
        process.stdin.close()
        process.wait(timeout=60)
    return {
        "latencyP50Ms": statistics.median(times),
        "latencyP95Ms": percentile(times, 0.95),
        "latencyMaxMs": max(times),
        "latencyMeanMs": statistics.fmean(times),
        "residentMb": resident,
        "peakMb": peak,
        "threads": threads_peak,
        "cpuMsPerCall": (ticks_after - ticks_before) * 1000.0 / CLOCK_TICKS / args.requests,
        "loadSeconds": loadSeconds,
        "artifactMb": size_mb(args.artifact),
    }


def check(gates, values):
    failures = []
    for name, bound in gates.get("performance", {}).get("metrics", {}).items():
        if name not in values:
            failures.append(f"{name}: no such metric")
            continue
        if "max" in bound and values[name] > bound["max"]:
            failures.append(f"{name}: {values[name]:.2f} above {bound['max']}")
        if "min" in bound and values[name] < bound["min"]:
            failures.append(f"{name}: {values[name]:.2f} below {bound['min']}")
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--decider", required=True)
    parser.add_argument("--gates")
    parser.add_argument("--artifact", nargs="*", default=[])
    parser.add_argument("--requests", type=int, default=200)
    parser.add_argument("--warmup", type=int, default=20)
    parser.add_argument("--busy-limit", type=float, default=0.20)
    parser.add_argument("--allow-busy", action="store_true")
    parser.add_argument("--label", default="decider")
    parser.add_argument("--report")
    args = parser.parse_args()

    busy = busy_fraction(3.0)
    load = float(pathlib.Path("/proc/loadavg").read_text().split()[0])
    idle = busy <= args.busy_limit
    if not idle and not args.allow_busy:
        print(f"perf-eval: the machine is busy (cpu {busy:.0%}, load {load:.1f}); no number is recorded")
        return SKIP
    try:
        values = measure(args)
    except OSError as error:
        print(f"perf-eval: cannot start the decider: {error}")
        return SKIP
    values["idle"] = idle
    print(f"{args.label}: " + ("idle machine" if idle else f"BUSY machine (cpu {busy:.0%}, load {load:.1f}): NOT REPORTABLE"))
    for name in ("latencyP50Ms", "latencyP95Ms", "latencyMaxMs", "cpuMsPerCall", "residentMb", "peakMb", "threads",
                 "artifactMb", "loadSeconds"):
        print(f"  {name:16s}{values[name]:10.2f}")
    failures = []
    if args.gates:
        failures = check(json.loads(pathlib.Path(args.gates).read_text()), values)
        if not idle:
            failures.append("measured on a busy machine: the gate is not judged")
    if args.report:
        pathlib.Path(args.report).write_text(json.dumps({"label": args.label, "metrics": values,
                                                         "failures": failures}, indent=2, sort_keys=True) + "\n")
    if failures:
        print("GATE FAILED")
        for failure in failures:
            print("  " + failure)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
