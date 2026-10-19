#!/usr/bin/env python3

import argparse
import glob
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

FINDING = re.compile(
    r"^(.+?):(\d+):(\d+): (warning|error): (.*) "
    r"\[([A-Za-z0-9_.\-]+(?:, *[A-Za-z0-9_.\-]+)*)\]$")
VERSION = re.compile(r"version (\d+)\.(\d+)\.(\d+)")


def database_dirs(root):
    pattern = f"{root}/*/build/*/compile_commands.json"
    deeper = f"{root}/*/*/build/*/compile_commands.json"
    return sorted(set(glob.glob(pattern) + glob.glob(deeper) +
                      glob.glob(f"{root}/*/*/*/build/*/compile_commands.json")))


def translation_units(root):
    pairs, seen, stale = [], set(), Counter()
    for database in database_dirs(root):
        try:
            entries = json.load(open(database))
        except (OSError, ValueError):
            continue
        directory = os.path.dirname(database)
        for entry in entries:
            path = entry.get("file", "")
            if "third_party" in path or "/build/" in path or \
                    not path.startswith(root) or path in seen:
                continue
            if not os.path.exists(path):
                stale[os.path.relpath(database, root)] += 1
                continue
            seen.add(path)
            pairs.append((directory, path, entry))
    return pairs, stale


def checks_of(root):
    config = os.path.join(root, ".clang-tidy")
    if not os.path.exists(config):
        return None
    text = open(config, encoding="utf-8").read()
    match = re.search(r"^Checks:(.*?)(?=^\S|\Z)", text, re.S | re.M)
    if not match:
        return None
    body = re.sub(r"^\s*[|>][-+]?", "", match.group(1), count=1)
    return "".join(body.split())


def tool_version(path):
    try:
        done = subprocess.run([path, "--version"], capture_output=True,
                              text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None, None
    match = VERSION.search(done.stdout + done.stderr)
    if not match:
        return None, None
    return ".".join(match.groups()), int(match.group(1))


def find_tool(major):
    fallback = None
    names = ["clang-tidy"]
    if major is not None:
        names.append(f"clang-tidy-{major}")
    for name in names:
        path = shutil.which(name)
        if path is None:
            continue
        version, found = tool_version(path)
        if found == major:
            return path, version, found
        fallback = fallback or (path, version, found)
    return fallback or (None, None, None)


PREPROCESS_DROP = {"-c", "-MD", "-MMD"}
PREPROCESS_DROP_WITH_VALUE = {"-o", "-MF", "-MT", "-MQ"}


def compiler_arguments(entry):
    if "arguments" in entry:
        words = list(entry["arguments"])
    else:
        words = shlex.split(entry.get("command", ""))
    for index, word in enumerate(words):
        if os.path.basename(word) == "ccache":
            words = words[index + 1:]
            break
    kept, skip = [], False
    for word in words:
        if skip:
            skip = False
            continue
        if word in PREPROCESS_DROP_WITH_VALUE:
            skip = True
            continue
        if word in PREPROCESS_DROP or word.startswith("-gsplit-dwarf"):
            continue
        kept.append(word)
    return kept


def cache_key(job, context):
    directory, path, entry = job
    root = context["root"]
    command = compiler_arguments(entry)
    try:
        done = subprocess.run(command + ["-E"], cwd=entry.get("directory", directory),
                              capture_output=True, timeout=300)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if done.returncode != 0:
        return None
    digest = hashlib.sha256()
    for part in (context["version"], context["checks"], context["header_filter"],
                 context["config"], " ".join(command).replace(root, "")):
        digest.update(part.encode())
        digest.update(b"\0")
    digest.update(done.stdout.replace(root.encode(), b""))
    return digest.hexdigest()


def cached_tidy(job, checks, header_filter, tool, context):
    directory, path, entry = job
    root = context["root"]
    cache = context["cache"]
    key = cache_key(job, context) if cache else None
    if key:
        stored = os.path.join(cache, key[:2], key + ".json")
        try:
            with open(stored, encoding="utf-8") as cached:
                record = json.load(cached)
            return path, [(root + name, line, check, message)
                          for name, line, check, message in record], None
        except (OSError, ValueError):
            pass
    path, findings, failure = tidy((directory, path), checks, header_filter, tool)
    if key and failure is None:
        os.makedirs(os.path.dirname(stored), exist_ok=True)
        relative = [(name[len(root):] if name.startswith(root) else name,
                     line, check, message)
                    for name, line, check, message in findings]
        partial = stored + f".{os.getpid()}.tmp"
        with open(partial, "w", encoding="utf-8") as out:
            json.dump(relative, out)
        os.replace(partial, stored)
    return path, findings, failure


def tidy_cache_dir():
    configured = os.environ.get("ARGUS_TIDY_CACHE")
    if configured == "off":
        return None
    if configured:
        return configured
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(
        os.path.expanduser("~"), ".cache")
    return os.path.join(base, "argus", "tidy")


def tidy(job, checks, header_filter, tool):
    database, path = job
    command = [tool, "-p", database, path, "-quiet",
               "--checks=" + checks, "--header-filter=" + header_filter]
    try:
        done = subprocess.run(command, capture_output=True, text=True,
                             timeout=900)
    except (OSError, subprocess.TimeoutExpired) as failure:
        return path, [], str(failure)
    findings = []
    for line in (done.stdout + done.stderr).splitlines():
        match = FINDING.match(line)
        if match:
            for check in match.group(6).split(","):
                findings.append((match.group(1), int(match.group(2)),
                                 check.strip(), match.group(5)))
    if not findings and done.returncode != 0:
        tail = (done.stderr or done.stdout).strip().splitlines()
        return path, [], tail[-1] if tail else f"exit {done.returncode}"
    return path, findings, None


def scan_workers():
    configured = os.environ.get("ARGUS_TIDY_JOBS", "")
    if configured.isdigit() and int(configured) > 0:
        return int(configured)
    if hasattr(os, "sched_getaffinity"):
        return max(1, len(os.sched_getaffinity(0)))
    return max(1, os.cpu_count() or 1)


def scan(root, jobs, checks, header_filter, tool, version):
    distinct, failures = {}, []
    config = os.path.join(root, ".clang-tidy")
    context = {
        "root": root,
        "cache": tidy_cache_dir(),
        "version": version or "",
        "checks": checks,
        "header_filter": header_filter.replace(re.escape(root), ""),
        "config": open(config, encoding="utf-8").read() if os.path.exists(config) else "",
    }
    with ThreadPoolExecutor(max_workers=scan_workers()) as pool:
        for path, findings, failure in pool.map(
                lambda job: cached_tidy(job, checks, header_filter, tool, context),
                jobs):
            if failure:
                failures.append((path, failure))
            for finding in findings:
                distinct.setdefault(finding, 1)
    counts = Counter(finding[2] for finding in distinct)
    files = Counter(finding[0][len(root) + 1:] for finding in distinct)
    return counts, files, failures


def read_baseline(path):
    baseline, tool = {}, None
    if not os.path.exists(path):
        return baseline, tool
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        name, _, value = line.partition(" ")
        value = value.strip()
        if name == "tool":
            tool = value
        elif value.isdigit():
            baseline[name] = int(value)
    return baseline, tool


def main():
    parser = argparse.ArgumentParser(description="Rules 16 and 19, measured.")
    parser.add_argument("--root", default=os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    parser.add_argument("--baseline", default=None)
    parser.add_argument("--write-baseline", action="store_true",
                        help="record what the tree has right now")
    parser.add_argument("--top", type=int, default=0,
                        help="print the N files with the most findings")
    arguments = parser.parse_args()
    root = os.path.abspath(arguments.root)
    baseline_path = arguments.baseline or os.path.join(
        root, "scripts", "lib", "tidy-baseline.txt")

    checks = checks_of(root)
    if checks is None:
        print(f"check-tidy: no Checks: list in {root}/.clang-tidy",
              file=sys.stderr)
        return 2

    baseline, recorded = read_baseline(baseline_path)
    major = int(recorded.split(".")[0]) if recorded and \
        recorded.split(".")[0].isdigit() else None
    tool, version, found = find_tool(major)
    if tool is None:
        print("check-tidy: clang-tidy is required", file=sys.stderr)
        return 2
    if major is not None and found != major and not arguments.write_baseline:
        print(f"check-tidy: the baseline was measured with clang-tidy "
              f"{recorded} and this is {version} ({tool}); the counts belong "
              f"to the tool, so install clang-tidy {major} or re-measure "
              f"deliberately with --write-baseline", file=sys.stderr)
        return 2
    if not recorded and not arguments.write_baseline:
        print(f"check-tidy: {baseline_path} records no clang-tidy version, so "
              f"the counts it holds cannot be compared with the {version} "
              f"found here; write a baseline with --write-baseline first",
              file=sys.stderr)
        return 2

    jobs, stale = translation_units(root)
    if not jobs:
        print("check-tidy: no compile_commands.json found -- build first",
              file=sys.stderr)
        return 2
    for database, count in sorted(stale.items()):
        print(f"check-tidy: {count} entries skipped in {database} -- the "
              f"database names files that are gone; delete that build tree",
              file=sys.stderr)
    counts, files, failures = scan(root, jobs, checks,
                                   r"^" + re.escape(root) + r"/(packages|services)",
                                   tool, version)

    if arguments.write_baseline:
        if failures:
            for path, failure in failures[:20]:
                print(f"unread: {path}  {failure}", file=sys.stderr)
            print(f"check-tidy: {len(failures)} of {len(jobs)} TUs could not be "
                  f"analysed, so no baseline was written", file=sys.stderr)
            return 1
        os.makedirs(os.path.dirname(baseline_path), exist_ok=True)
        with open(baseline_path, "w", encoding="utf-8") as out:
            out.write(f"tool {version}\n")
            out.write(f"tus {len(jobs)}\n")
            for check, count in sorted(counts.items(), key=lambda kv: (-kv[1],
                                                                      kv[0])):
                out.write(f"{check} {count}\n")
        print(f"check-tidy: baseline written to {baseline_path} "
              f"({len(jobs)} TUs, {sum(counts.values())} findings)")

    if arguments.top:
        print("=== files with the most findings ===")
        for name, count in files.most_common(arguments.top):
            print(f"{count:5d}  {name}")

    if arguments.write_baseline:
        if recorded and recorded != version:
            print(f"check-tidy: the previous baseline was measured with "
                  f"clang-tidy {recorded}", file=sys.stderr)
        print(f"check-tidy: clang-tidy {version} ({tool})")
        return 0
    if not baseline:
        print("check-tidy: no baseline recorded yet", file=sys.stderr)
        return 1

    risen = [(check, count, baseline.get(check, 0))
             for check, count in counts.items()
             if count > baseline.get(check, 0)]
    fallen = [(check, count, baseline[check]) for check, count in counts.items()
              if check in baseline and count < baseline[check]]

    for check, count, was in sorted(risen):
        print(f"risen: {check}  {count} findings, baseline {was}",
              file=sys.stderr)
    if len(jobs) < baseline.get("tus", 0):
        print(f"risen: the scan saw {len(jobs)} TUs, baseline "
              f"{baseline['tus']} -- build the whole tree before the gate",
              file=sys.stderr)
        risen.append(("tus", len(jobs), baseline["tus"]))
    for path, failure in failures[:20]:
        print(f"unread: {path}  {failure}", file=sys.stderr)

    print(f"check-tidy: clang-tidy {version} ({tool})")
    print(f"check-tidy: {len(jobs)} TUs, {sum(counts.values())} findings over "
          f"{len(counts)} checks, baseline "
          f"{sum(v for k, v in baseline.items() if k != 'tus')}"
          + (f"; {len(fallen)} checks below it" if fallen else ""))
    if arguments.top == 0 and files:
        worst = files.most_common(1)[0]
        print(f"check-tidy: worst file {worst[0]} ({worst[1]} findings)")
    return 1 if risen or failures else 0


if __name__ == "__main__":
    sys.exit(main())
