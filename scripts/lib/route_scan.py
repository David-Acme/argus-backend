#!/usr/bin/env python3

import argparse
import sys
from pathlib import Path


ROUTE_MACROS = ("ADD_METHOD_TO", "WS_PATH_ADD")
RAW_MACRO = "registerHandler"
REGEX_FORMS = (
    "ADD_METHOD_VIA_REGEX",
    "WS_ADD_PATH_VIA_REGEX",
    "registerHttpControllerViaRegex",
    "registerWebSocketControllerRegex",
    "registerHandlerViaRegex",
)
FILTERS = ("DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter")
ORDER = {name: index for index, name in enumerate(FILTERS)}
MULTIPART_TOKENS = ("MultiPartParser", "form_multipart")
METHODS = ("Get", "Post", "Put", "Patch", "Delete", "Head", "Options")
PUBLIC_ROUTES = frozenset({
    ("auth", "POST", "/auth/device-login"),
    ("auth", "GET", "/auth/device-login/{1}"),
    ("auth", "POST", "/auth/login"),
    ("auth", "PATCH", "/auth/refresh-token"),
    ("auth", "POST", "/auth/register"),
    ("guard", "GET", "/health"),
    ("identity", "POST", "/invitation/resolve"),
    ("identity", "POST", "/pairing"),
    ("identity", "GET", "/pairing/status"),
    ("llm", "POST", "/llm/v1/chat"),
    ("llm", "POST", "/llm/v1/chat-stream"),
    ("llm", "GET", "/llm/v1/config"),
    ("shared", "GET", "/health"),
    ("stt", "GET", "/stt/v1/config"),
    ("stt", "POST", "/stt/v1/transcribe"),
    ("tts", "GET", "/tts/v1/config"),
    ("tts", "POST", "/tts/v1/synthesize"),
    ("tts", "POST", "/tts/v1/synthesize-stream"),
    ("vlm", "GET", "/vlm/v1/config"),
    ("vlm", "POST", "/vlm/v1/describe"),
})


class Unclassified(Exception):
    pass


def skip_literal(source, index):
    quote = source[index]
    index += 1
    while index < len(source):
        if source[index] == "\\":
            index += 2
            continue
        if source[index] == quote:
            return index + 1
        index += 1
    raise Unclassified("unterminated literal")


def skip_balanced(source, index):
    pairs = {"(": ")", "[": "]", "{": "}"}
    closer = pairs[source[index]]
    depth = 0
    while index < len(source):
        char = source[index]
        if char in "\"'":
            index = skip_literal(source, index)
            continue
        if char in pairs:
            depth += 1
        elif char in (")", "]", "}"):
            depth -= 1
            if depth == 0:
                if char != closer:
                    raise Unclassified("mismatched brackets")
                return index + 1
        index += 1
    raise Unclassified("unbalanced brackets")


def split_arguments(text):
    parts = []
    start = 0
    index = 0
    while index < len(text):
        char = text[index]
        if char in "\"'":
            index = skip_literal(text, index)
            continue
        if char in "([{":
            index = skip_balanced(text, index)
            continue
        if char == ",":
            parts.append(text[start:index])
            start = index + 1
        index += 1
    tail = text[start:]
    if tail.strip() or parts:
        parts.append(tail)
    return [part.strip() for part in parts if part.strip()]


def unquote(token):
    token = token.strip()
    if len(token) >= 2 and token[0] == '"' and token[-1] == '"':
        return token[1:-1]
    return None


def methods_of(argument):
    found = []
    for token in split_arguments(argument.replace("{", "").replace("}", "")):
        name = token.strip().split("::")[-1]
        if name in METHODS:
            found.append(name.upper())
    return found


def find_braces(source, start):
    depth = 0
    index = start
    while index < len(source):
        char = source[index]
        if char in "\"'":
            index = skip_literal(source, index)
            continue
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return index + 1
        index += 1
    return len(source)


def handler_body(cpp_text, handler):
    name = handler.split("::")[-1]
    needle = "::" + name + "("
    index = cpp_text.find(needle)
    while index != -1:
        open_paren = index + len(needle) - 1
        close_paren = skip_balanced(cpp_text, open_paren)
        brace = cpp_text.find("{", close_paren)
        if brace != -1:
            return cpp_text[brace:find_braces(cpp_text, brace)]
        index = cpp_text.find(needle, index + 1)
    return ""


def find_identifier(text, name, start=0):
    index = text.find(name, start)
    while index != -1:
        before = text[index - 1] if index > 0 else ""
        end = index + len(name)
        after = text[end] if end < len(text) else ""
        if not (before.isalnum() or before == "_") \
                and not (after.isalnum() or after == "_"):
            return index
        index = text.find(name, index + 1)
    return -1


def find_call(text, name, start=0):
    index = find_identifier(text, name, start)
    while index != -1:
        if text[index + len(name):index + len(name) + 1] == "(":
            return index
        index = find_identifier(text, name, index + 1)
    return -1


def reject_regex_forms(path, text):
    for name in REGEX_FORMS:
        if find_identifier(text, name) != -1:
            raise Unclassified(
                f"{path}: {name} registers routes through a pattern this "
                f"census cannot spell, so its routes would be invisible here")


def source_files(root):
    for parent in ("services", "packages"):
        base = root / parent
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix in (".hxx", ".cc") and "/src/" in str(path):
                yield path


def unit_of(path, root):
    relative = path.relative_to(root)
    parts = relative.parts
    if parts[0] == "services":
        return parts[1]
    return "shared"


def scan_file(path, root):
    text = path.read_text()
    reject_regex_forms(path, text)
    companion = None
    if path.suffix == ".hxx":
        candidate = path.with_suffix(".cc")
        if candidate.is_file():
            companion = candidate.read_text()
    records = []
    for macro in ROUTE_MACROS + (RAW_MACRO,):
        index = find_call(text, macro)
        while index != -1:
            open_paren = index + len(macro)
            try:
                close_paren = skip_balanced(text, open_paren)
            except Unclassified as error:
                raise Unclassified(f"{path}: {macro}: {error}")
            inner = text[open_paren + 1:close_paren - 1]
            records.append(build_record(path, root, macro, inner, text,
                                        companion))
            index = find_call(text, macro, close_paren)
    return records


def build_record(path, root, macro, inner, text, companion):
    arguments = split_arguments(inner)
    literals = [value for value in (unquote(arg) for arg in arguments)
                if value is not None]
    if not literals:
        raise Unclassified(f"{path}: {macro} carries no path literal")
    record = {
        "unit": unit_of(path, root),
        "file": str(path.relative_to(root)),
        "path": literals[0],
        "filters": list(literals[1:]),
    }
    if macro == "ADD_METHOD_TO":
        record["handler"] = arguments[0].strip()
        record["methods"] = methods_of(",".join(arguments[2:]))
        record["kind"] = "http"
    elif macro == "WS_PATH_ADD":
        record["handler"] = "websocket"
        record["methods"] = ["WS"]
        record["kind"] = "websocket"
    else:
        record["handler"] = "lambda"
        record["methods"] = methods_of(arguments[2]) if len(arguments) > 2 else []
        record["kind"] = "raw"
    if not record["methods"]:
        raise Unclassified(f"{path}: {macro} {record['path']} names no method")
    body = inner if macro == RAW_MACRO else handler_body(
        companion or text, record["handler"])
    record["multipart"] = any(token in body for token in MULTIPART_TOKENS)
    return record


def scan(root):
    records = []
    for path in source_files(root):
        records.extend(scan_file(path, root))
    records.sort(key=lambda row: (row["unit"], row["path"], ",".join(row["methods"])))
    return records


def line_of(record):
    filters = ",".join(record["filters"]) or "-"
    return "\t".join([
        record["unit"],
        "/".join(record["methods"]),
        record["path"],
        filters,
        "multipart" if record["multipart"] else "-",
        record["file"],
    ])


def load_baseline(path):
    if not path.is_file():
        return []
    return [line for line in path.read_text().splitlines() if line.strip()]


def rules(records):
    problems = []
    seen = {}
    for record in records:
        where = f"{record['unit']} {'/'.join(record['methods'])} {record['path']}"
        names = [name for name in record["filters"] if name in ORDER]
        positions = [ORDER[name] for name in names]
        if positions != sorted(positions):
            problems.append(f"out of order: {where}  {','.join(record['filters'])}")
        if "RoleFilter" in names and "JwtFilter" not in names:
            problems.append(f"RoleFilter without JwtFilter: {where}")
        if "JwtFilter" in names and "DeviceFilter" not in names:
            problems.append(f"JwtFilter without DeviceFilter: {where}")
        public_key = (record["unit"], "/".join(record["methods"]), record["path"])
        if "JwtFilter" not in names and public_key not in PUBLIC_ROUTES:
            problems.append(f"unauthenticated route outside the public list: {where}")
        if "JwtFilter" in names and public_key in PUBLIC_ROUTES:
            problems.append(f"public-listed route behind JwtFilter: {where}")
        if record["multipart"] and "ValidJsonFilter" in names:
            problems.append(
                f"multipart handler behind ValidJsonFilter: {where}")
        unknown = [name for name in record["filters"] if name not in ORDER]
        for name in unknown:
            problems.append(f"unknown filter {name}: {where}")
        key = (record["unit"], "/".join(record["methods"]), record["path"])
        if key in seen:
            problems.append(
                f"duplicate route: {where}  {seen[key]} and {record['file']}")
        seen[key] = record["file"]
    return problems


def main():
    parser = argparse.ArgumentParser(
        description="Census every HTTP and WebSocket route the tree "
                    "declares and hold it against a pinned baseline.")
    parser.add_argument("--root", required=True)
    parser.add_argument("--baseline", default="scripts/lib/route-baseline.txt")
    parser.add_argument("--update", action="store_true",
                        help="rewrite the baseline from the tree")
    parser.add_argument("--list", action="store_true",
                        help="print the census")
    arguments = parser.parse_args()
    root = Path(arguments.root).resolve()
    baseline_path = root / arguments.baseline

    try:
        records = scan(root)
    except Unclassified as error:
        print(f"check-routes: unclassified registration  {error}",
              file=sys.stderr)
        return 1

    lines = [line_of(record) for record in records]
    if arguments.list:
        for line in lines:
            print(line)

    problems = rules(records)
    for problem in problems:
        print(f"forbidden: {problem}", file=sys.stderr)

    baseline = load_baseline(baseline_path)
    if arguments.update:
        baseline_path.write_text("\n".join(lines) + "\n")
        baseline = lines

    added = [line for line in lines if line not in baseline]
    removed = [line for line in baseline if line not in lines]
    for line in added:
        print(f"added: {line}", file=sys.stderr)
    for line in removed:
        print(f"removed: {line}", file=sys.stderr)

    guarded = sum(1 for row in records if "JwtFilter" in row["filters"])
    multipart = sum(1 for row in records if row["multipart"])
    units = len({row["unit"] for row in records})
    print(f"check-routes: {len(records)} declarations over {units} units, "
          f"{guarded} behind JwtFilter, {multipart} multipart, "
          f"{len(added)} added, {len(removed)} removed, "
          f"{len(problems)} forbidden")
    return 1 if problems or added or removed else 0


if __name__ == "__main__":
    sys.exit(main())
