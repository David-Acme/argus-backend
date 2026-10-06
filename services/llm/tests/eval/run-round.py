#!/usr/bin/env python3
import argparse
import pathlib
import shlex
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
SKIP = 77
TIMED_OUT = (124, 137)
STAGES = ("fill", "score", "perf", "report")


def execute(command, log=None):
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    lines = []
    for line in process.stdout:
        sys.stdout.write(line)
        lines.append(line)
    code = process.wait()
    if log:
        pathlib.Path(log).write_text("".join(lines))
    return code


def wrapped(args, command):
    return [*shlex.split(args.runner), "timeout", "--kill-after=30", str(args.seconds), *command]


def harness_command(args, *extra):
    command = [sys.executable, "-I", str(HERE / "decider-eval.py"), "--decider", args.decider, "--gates", args.gates,
               "--select", *args.select]
    if args.negatives:
        command += ["--select-negatives", args.negatives]
    return command + list(extra)


def fill(args, out):
    for index in range(args.chunks):
        code = execute(wrapped(args, harness_command(args, "--cache", str(out / "cache.json"),
                                                     "--chunk", f"{index}/{args.chunks}")))
        if code in TIMED_OUT:
            print(f"run-round: chunk {index}/{args.chunks} used more than {args.seconds} s; the chunks already "
                  f"finished are in {out / 'cache.json'}, so run again with more --chunks")
            return 1
        if code == SKIP:
            return SKIP
        if code != 0:
            print(f"run-round: chunk {index}/{args.chunks} failed with exit code {code}")
            return 1
    return 0


def score(args, out):
    common = ["--cache", str(out / "cache.json"), "--latency", "0", "--guard-scope", args.guard_scope]
    traffic = ["--traffic", *args.traffic] if args.traffic else []
    sliced = ["--slices", args.slices] if args.slices else []
    passes = (("uncalibrated", ["--calibrate-out", str(out / "calibration.json"),
                                "--errors", str(out / "errors-uncalibrated.jsonl")] + traffic),
              ("calibrated", ["--calibration", str(out / "calibration.json"), "--errors", str(out / "errors.jsonl")]
               + traffic + sliced))
    for name, extra in passes:
        command = harness_command(args, *common, *extra, "--report", str(out / f"{name}.json"))
        code = execute(wrapped(args, command), out / f"{name}.txt")
        if code not in (0, 1):
            print(f"run-round: the {name} scoring pass failed with exit code {code}")
            return 1
    return 0


def perf(args, out):
    command = [sys.executable, "-I", str(HERE / "perf-eval.py"), "--decider", args.decider, "--gates", args.gates,
               "--label", args.name, "--report", str(out / "perf.json")]
    if args.artifact:
        command += ["--artifact", *args.artifact]
    code = execute(wrapped(args, command), out / "perf.txt")
    if code == SKIP:
        print("run-round: the machine is busy, so no performance number was recorded")
        return SKIP
    return 0 if code in (0, 1) else 1


def report(args, out):
    command = [sys.executable, "-I", str(HERE / "round-report.py"), "--round", f"{args.name}={out}"]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        return 1
    (out / "report.md").write_text(result.stdout)
    sys.stdout.write(result.stdout)
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--decider", required=True)
    parser.add_argument("--select", nargs="+", required=True)
    parser.add_argument("--negatives")
    parser.add_argument("--slices")
    parser.add_argument("--traffic", nargs="*", default=[])
    parser.add_argument("--artifact", nargs="*", default=[])
    parser.add_argument("--gates", default=str(HERE / "gates.json"))
    parser.add_argument("--runner", default="")
    parser.add_argument("--chunks", type=int, default=8)
    parser.add_argument("--seconds", type=int, default=840)
    parser.add_argument("--guard-scope", choices=["both", "all", "low-risk-open"], default="both")
    parser.add_argument("--stages", default=",".join(STAGES))
    args = parser.parse_args()
    chosen = [stage for stage in args.stages.split(",") if stage]
    unknown = [stage for stage in chosen if stage not in STAGES]
    if unknown or args.chunks < 1:
        parser.error(f"unknown stage {unknown} or fewer than one chunk")
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    runners = {"fill": fill, "score": score, "perf": perf, "report": report}
    status = 0
    for stage in chosen:
        print(f"\n=== {args.name}: {stage}")
        code = runners[stage](args, out)
        if code == SKIP and stage == "perf":
            status = status or SKIP
        elif code != 0:
            return code
    return status


if __name__ == "__main__":
    sys.exit(main())
