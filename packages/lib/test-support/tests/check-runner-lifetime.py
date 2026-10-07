import argparse
import re
import sys
from pathlib import Path

HELPER_INCLUDE = "test-support/app-runner.hxx"
SUFFIXES = {".cc", ".hxx"}
EARLY_FLAG_QUIT = re.compile(r"\bif\s*\(\s*drogon::app\(\)\.isRunning\(\)\s*\)\s*\{[^{}]*?drogon::app\(\)\.quit\(\)")
STATIC_RUNNER = re.compile(r"\bstatic\b[^;{}]*\bAppRunner\b")
STATIC_LAMBDA = re.compile(r"\bstatic\b[^;{}]*=\s*\[[^\]]*\]\s*(?:\([^)]*\))?\s*(?:->[^{]*)?\{")
CLASS_HEAD = re.compile(r"\b(?:class|struct)\s+(\w+)[^;{]*\{")


def matching_brace(text, opening):
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    return len(text)


def holder_classes(text):
    names = set()
    for head in CLASS_HEAD.finditer(text):
        name = head.group(1)
        if name == "AppRunner":
            continue
        body = text[head.end() - 1 : matching_brace(text, head.end() - 1)]
        if "AppRunner" in body:
            names.add(name)
    return names


def line_of(text, index):
    return text.count("\n", 0, index) + 1


def findings(text):
    found = []
    for match in EARLY_FLAG_QUIT.finditer(text):
        found.append((line_of(text, match.start()), "a quit guarded by the early isRunning flag, which Drogon sets before its main loop runs"))
    if HELPER_INCLUDE in text and "class AppRunner" not in text:
        return sorted(found)
    for match in STATIC_RUNNER.finditer(text):
        found.append((line_of(text, match.start()), "a static AppRunner"))
    for match in STATIC_LAMBDA.finditer(text):
        body = text[match.end() - 1 : matching_brace(text, match.end() - 1)]
        if "AppRunner" in body:
            found.append((line_of(text, match.start()), "a static initializer that builds an AppRunner"))
    for name in sorted(holder_classes(text)):
        for match in re.finditer(r"\bstatic\s+(?:const\s+)?" + re.escape(name) + r"\b[^;{}]*;", text):
            found.append((line_of(text, match.start()), f"a static {name}, which holds an AppRunner"))
    return sorted(found)


def sources(root):
    patterns = ["services/*/tests", "packages/*/*/tests"]
    for pattern in patterns:
        for directory in sorted(root.glob(pattern)):
            for path in sorted(directory.rglob("*")):
                if path.suffix in SUFFIXES and "fixtures" not in path.parts:
                    yield path


def scan(root):
    failures = []
    for path in sources(root):
        for line, reason in findings(path.read_text()):
            failures.append(f"{path.relative_to(root)}:{line}: {reason}")
    return failures


def self_test(directory):
    problems = []
    for path in sorted(directory.glob("*.cc")):
        found = findings(path.read_text())
        if path.name.startswith("bad-") and not found:
            problems.append(f"{path.name} should have been flagged")
        if path.name.startswith("good-") and found:
            problems.append(f"{path.name} should not have been flagged: {found}")
    if not list(directory.glob("bad-*.cc")) or not list(directory.glob("good-*.cc")):
        problems.append("the fixtures hold no bad sample or no good sample")
    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root")
    parser.add_argument("--self-test")
    arguments = parser.parse_args()
    if arguments.self_test:
        problems = self_test(Path(arguments.self_test))
        for problem in problems:
            print(problem, file=sys.stderr)
        print(f"runner lifetime self-test: {len(problems)} problems")
        return 1 if problems else 0
    failures = scan(Path(arguments.root).resolve())
    for failure in failures:
        print(failure, file=sys.stderr)
    print(f"runner lifetime: {len(failures)} runners to fix")
    return 1 if failures else 0


sys.exit(main())
