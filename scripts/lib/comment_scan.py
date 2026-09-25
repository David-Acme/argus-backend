#!/usr/bin/env python3
import argparse
import ast
import io
import re
import subprocess
import sys
import tokenize
from pathlib import Path


class ScanError(Exception):
    pass


IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
RAW_PREFIXES = {"R", "u8R", "uR", "UR", "LR"}
CMAKE_BRACKET = re.compile(r"\[(=*)\[")
CMAKE_BRACKET_COMMENT = re.compile(r"#\[(=*)\[")
HEREDOC = re.compile(r"<<(-?)[ \t]*(?:'([^'\n]*)'|\"([^\"\n]*)\"|\\?([^\s;&|<>()'\"]+))")
DOCKER_DIRECTIVE = re.compile(r"#\s*[A-Za-z]+\s*=")
DOCKER_HEREDOC = re.compile(r"<<(-?)[\"']?([A-Za-z_][A-Za-z0-9_]*)[\"']?")
YAML_BLOCK = re.compile(r"(?:^|[:?]\s+|^-\s*|\s-\s+)(?:[&!]\S*\s+)*[|>][+-]?[0-9]?[+-]?\s*$")
PY_STATEMENT_TAIL = re.compile(r"[ \t]*;[ \t]*")
SHELL_SEPARATORS = " \t\n;&|()<>"
UNSPACED_NEIGHBOURS = "()[]{},;"

EXCLUDED_PREFIXES = ("third_party/", "docs/", "packages/contracts/proto/grpc/")
FIRST_PARTY_UNDER_EXCLUDED = {"third_party/.gitignore", "third_party/sqlite-vec/CMakeLists.txt"}
EXCLUDED_PARTS = ("/tests/fixtures/",)
NOT_CODE_SUFFIXES = {".md", ".tsv", ".bin", ".sha256", ".png", ".jpg"}
NOT_CODE_NAMES = {"LICENSE", "NOTICE"}
HASH_NAMES = {".gitignore", ".dockerignore", "conanfile.txt",
              "tidy-baseline.txt", "route-baseline.txt"}

OPENERS = {
    "cpp": ("{", "(", "["),
    "proto": ("{", "("),
    "cmake": ("(",),
    "python": (":", "(", "[", "{"),
    "shell": ("{", "(", "then", "do", "else", "in"),
}
CLOSERS = {
    "cpp": ("}", ")", "]"),
    "proto": ("}", ")"),
    "cmake": (")",),
    "python": (")", "]", "}"),
    "shell": ("}", ")", "fi", "done", "esac", "else", "elif", ";;"),
}


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def end_of_line(text, i):
    j = text.find("\n", i)
    j = len(text) if j < 0 else j
    return j - 1 if j > i and text[j - 1] == "\r" else j


def iter_lines(text):
    pos = 0
    for raw in text.split("\n"):
        yield pos, raw[:-1] if raw.endswith("\r") else raw
        pos += len(raw) + 1


def skip_quoted(text, i, quote, multiline):
    j = i + 1
    n = len(text)
    while j < n:
        c = text[j]
        if c == "\\":
            j += 2
            continue
        if c == quote:
            return j + 1
        if c == "\n" and not multiline:
            break
        j += 1
    raise ScanError(f"unterminated {quote} literal at line {line_of(text, i)}")


def c_family_spans(text, proto):
    spans = []
    n = len(text)
    i = 0
    last_ident = ""
    last_ident_end = -1
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = i + 2
            while j < n and not (text[j] == "\n" and text[j - 1] != "\\"):
                j += 1
            if j > i and text[j - 1] == "\r":
                j -= 1
            spans.append((i, j))
            i = j
            continue
        if c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            if j < 0:
                raise ScanError(f"unterminated block comment at line {line_of(text, i)}")
            spans.append((i, j + 2))
            i = j + 2
            continue
        if c == '"':
            if not proto and last_ident_end == i and last_ident in RAW_PREFIXES:
                open_paren = text.find("(", i + 1)
                delim = text[i + 1:open_paren] if open_paren >= 0 else ""
                if open_paren < 0 or len(delim) > 16 or any(ch in delim for ch in " \\\n)"):
                    raise ScanError(f"malformed raw string at line {line_of(text, i)}")
                close = text.find(")" + delim + '"', open_paren + 1)
                if close < 0:
                    raise ScanError(f"unterminated raw string at line {line_of(text, i)}")
                i = close + len(delim) + 2
            else:
                i = skip_quoted(text, i, '"', False)
            continue
        if c == "'":
            i = skip_quoted(text, i, "'", False)
            continue
        if c.isdigit() or (c == "." and nxt.isdigit()):
            j = i + 1
            while j < n:
                d = text[j]
                if d.isalnum() or d in "_.":
                    j += 1
                elif d == "'" and not proto and j + 1 < n and text[j + 1].isalnum():
                    j += 1
                elif d in "+-" and text[j - 1] in "eEpP":
                    j += 1
                else:
                    break
            i = j
            continue
        m = IDENT.match(text, i)
        if m:
            last_ident = m.group(0)
            last_ident_end = m.end()
            i = m.end()
            continue
        i += 1
    return spans


def cmake_spans(text):
    spans = []
    n = len(text)
    i = 0
    while i < n:
        c = text[i]
        if c == "#":
            m = CMAKE_BRACKET_COMMENT.match(text, i)
            if m:
                close = "]" + m.group(1) + "]"
                j = text.find(close, m.end())
                if j < 0:
                    raise ScanError(f"unterminated bracket comment at line {line_of(text, i)}")
                spans.append((i, j + len(close)))
                i = j + len(close)
                continue
            j = end_of_line(text, i)
            spans.append((i, j))
            i = j
            continue
        if c == '"':
            i = skip_quoted(text, i, '"', True)
            continue
        if c == "\\":
            i += 2
            continue
        if c == "[" and (i == 0 or text[i - 1] in " \t\n("):
            m = CMAKE_BRACKET.match(text, i)
            if m:
                close = "]" + m.group(1) + "]"
                j = text.find(close, m.end())
                if j < 0:
                    raise ScanError(f"unterminated bracket argument at line {line_of(text, i)}")
                i = j + len(close)
                continue
        i += 1
    return spans


def skip_ansi_c(text, i):
    j = i + 1
    while j < len(text):
        if text[j] == "\\":
            j += 2
            continue
        if text[j] == "'":
            return j + 1
        j += 1
    raise ScanError(f"unterminated $' at line {line_of(text, i)}")


def consume_heredocs(text, i, pending):
    n = len(text)
    for delim, strip_tabs in pending:
        while True:
            if i >= n:
                raise ScanError(f"unterminated heredoc {delim}")
            end = text.find("\n", i)
            end = n if end < 0 else end
            body_line = text[i:end].rstrip("\r")
            if strip_tabs:
                body_line = body_line.lstrip("\t")
            i = end + 1
            if body_line == delim:
                break
    return i


def shell_spans(text):
    spans = []
    n = len(text)
    i = 0
    frames = [["code", 0, 0]]
    pending = []
    sep = True
    while i < n:
        frame = frames[-1]
        kind = frame[0]
        c = text[i]
        if c == "\n" and pending and kind in ("code", "cmd", "bq", "arith"):
            i = consume_heredocs(text, i + 1, pending)
            pending = []
            sep = True
            continue
        if kind in ("code", "cmd", "bq"):
            if c == "#" and sep:
                j = end_of_line(text, i)
                if kind == "bq":
                    k = text.find("`", i, j)
                    j = k if k >= 0 else j
                if not (i == 0 and text.startswith("#!")):
                    spans.append((i, j))
                i = j
                continue
            if c == "\\":
                i += 2
                sep = False
                continue
            if text.startswith("$'", i):
                i = skip_ansi_c(text, i + 1)
                sep = False
                continue
            if c == "'":
                j = text.find("'", i + 1)
                if j < 0:
                    raise ScanError(f"unterminated ' at line {line_of(text, i)}")
                i = j + 1
                sep = False
                continue
            if c == '"':
                frames.append(["dq", 0, 0])
                i += 1
                sep = False
                continue
            if c == "`":
                if kind == "bq":
                    frames.pop()
                    sep = False
                else:
                    frames.append(["bq", 0, 0])
                    sep = True
                i += 1
                continue
            if text.startswith("$((", i):
                frames.append(["arith", 0, 0])
                i += 3
                sep = False
                continue
            if sep and text.startswith("((", i):
                frames.append(["arith", 0, 0])
                i += 2
                sep = False
                continue
            if text.startswith("$(", i):
                frames.append(["cmd", 0, 0])
                i += 2
                sep = True
                continue
            if text.startswith("${", i):
                frames.append(["param", 0, 0])
                i += 2
                sep = False
                continue
            if text.startswith("<<<", i):
                i += 3
                sep = True
                continue
            if text.startswith("<<", i):
                m = HEREDOC.match(text, i)
                if m:
                    delim = next(g for g in m.groups()[1:] if g is not None)
                    pending.append((delim, m.group(1) == "-"))
                    i = m.end()
                    sep = False
                    continue
            if sep:
                word = IDENT.match(text, i)
                if word:
                    after = text[word.end()] if word.end() < n else "\n"
                    if after in SHELL_SEPARATORS:
                        if word.group(0) == "case":
                            frame[2] += 1
                        elif word.group(0) == "esac" and frame[2] > 0:
                            frame[2] -= 1
                    i = word.end()
                    sep = False
                    continue
            if kind == "cmd" and c == ")":
                if frame[1] > 0:
                    frame[1] -= 1
                    sep = True
                elif frame[2] > 0:
                    sep = True
                else:
                    frames.pop()
                    sep = False
                i += 1
                continue
            if kind == "cmd" and c == "(":
                frame[1] += 1
            sep = c in SHELL_SEPARATORS
            i += 1
            continue
        if kind == "dq":
            if c == "\\":
                i += 2
                continue
            if c == '"':
                frames.pop()
                i += 1
                sep = False
                continue
            if text.startswith("$((", i):
                frames.append(["arith", 0, 0])
                i += 3
                continue
            if text.startswith("$(", i):
                frames.append(["cmd", 0, 0])
                i += 2
                sep = True
                continue
            if text.startswith("${", i):
                frames.append(["param", 0, 0])
                i += 2
                continue
            if c == "`":
                frames.append(["bq", 0, 0])
                sep = True
            i += 1
            continue
        if kind == "param":
            if c == "\\":
                i += 2
                continue
            if c == "}":
                frames.pop()
                i += 1
                sep = False
                continue
            if c == "'" and frames[-2][0] != "dq":
                j = text.find("'", i + 1)
                if j < 0:
                    raise ScanError(f"unterminated ' at line {line_of(text, i)}")
                i = j + 1
                continue
            if c == '"':
                frames.append(["dq", 0, 0])
                i += 1
                continue
            if text.startswith("${", i):
                frames.append(["param", 0, 0])
                i += 2
                continue
            if text.startswith("$((", i):
                frames.append(["arith", 0, 0])
                i += 3
                continue
            if text.startswith("$(", i):
                frames.append(["cmd", 0, 0])
                i += 2
                sep = True
                continue
            i += 1
            continue
        if kind == "arith":
            if c == "(":
                frame[1] += 1
            elif c == ")":
                if frame[1] == 0 and text.startswith("))", i):
                    frames.pop()
                    i += 2
                    sep = False
                    continue
                frame[1] -= 1
            elif text.startswith("$(", i) and not text.startswith("$((", i):
                frames.append(["cmd", 0, 0])
                i += 2
                sep = True
                continue
            elif text.startswith("${", i):
                frames.append(["param", 0, 0])
                i += 2
                continue
            i += 1
            continue
        i += 1
    if pending:
        raise ScanError(f"unterminated heredoc {pending[0][0]}")
    if len(frames) != 1:
        raise ScanError(f"unbalanced shell quoting (ends inside {frames[-1][0]})")
    return spans


def python_spans(text):
    lines = text.split("\n")
    starts = [0]
    for line in lines:
        starts.append(starts[-1] + len(line) + 1)

    def offset(row, col_chars):
        return starts[row - 1] + col_chars

    def char_col(row, byte_col):
        return len(lines[row - 1].encode("utf-8")[:byte_col].decode("utf-8"))

    spans = []
    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(text, newline="").readline))
        tree = ast.parse(text)
    except (SyntaxError, tokenize.TokenError) as error:
        raise ScanError(f"python does not parse: {error}") from error
    if any(isinstance(node, ast.Name) and node.id == "__doc__" for node in ast.walk(tree)):
        raise ScanError("reads __doc__, which a docstring-free file does not have; pass the text explicitly")
    for tok in tokens:
        if tok.type != tokenize.COMMENT:
            continue
        if tok.start == (1, 0) and tok.string.startswith("#!"):
            continue
        spans.append((offset(*tok.start), offset(*tok.end)))
    for node in ast.walk(tree):
        if not isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        body = node.body
        if not body:
            continue
        first = body[0]
        if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant) and isinstance(first.value.value, str):
            if len(body) == 1 and not isinstance(node, ast.Module):
                raise ScanError(f"docstring is the only statement at line {first.lineno}")
            start = offset(first.lineno, char_col(first.lineno, first.col_offset))
            end = offset(first.end_lineno, char_col(first.end_lineno, first.end_col_offset))
            tail = PY_STATEMENT_TAIL.match(text, end)
            if tail and tail.group(0).strip():
                end = tail.end()
            spans.append((start, end))
    return sorted(spans)


def sql_spans(text):
    spans = []
    n = len(text)
    i = 0
    while i < n:
        c = text[i]
        if text.startswith("--", i):
            j = end_of_line(text, i)
            spans.append((i, j))
            i = j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                raise ScanError(f"unterminated block comment at line {line_of(text, i)}")
            spans.append((i, j + 2))
            i = j + 2
            continue
        if c in "'\"`":
            j = i + 1
            while True:
                k = text.find(c, j)
                if k < 0:
                    raise ScanError(f"unterminated {c} at line {line_of(text, i)}")
                if text.startswith(c * 2, k):
                    j = k + 2
                    continue
                i = k + 1
                break
            continue
        i += 1
    return spans


def full_line_spans(text, markers):
    spans = []
    for pos, line in iter_lines(text):
        stripped = line.lstrip()
        if stripped.startswith(markers):
            spans.append((pos + len(line) - len(stripped), pos + len(line)))
    return spans


def inline_comment(line, quote, openers, markers):
    k = 0
    n = len(line)
    while k < n:
        c = line[k]
        if quote:
            if c == "\\" and quote == '"':
                k += 2
                continue
            if c == quote:
                if quote == "'" and line.startswith("''", k):
                    k += 2
                    continue
                quote = None
            k += 1
            continue
        if c in "'\"" and openers(line, k):
            quote = c
            k += 1
            continue
        if c in markers and (k == 0 or line[k - 1] in " \t"):
            return k, None
        k += 1
    return None, quote


def yaml_quote_opener(line, k):
    before = line[:k].rstrip()
    return before == "" or before.endswith((":", "-", "[", "{", ",", "?"))


def env_quote_opener(line, k):
    return line[:k].endswith("=")


def gitconfig_quote_opener(line, k):
    return line[:k].rstrip().endswith("=")


def yaml_block_parent(code):
    col = len(code) - len(code.lstrip(" "))
    rest = code[col:]
    dash = None
    while rest.startswith("-") and (len(rest) == 1 or rest[1] in " \t"):
        dash = col
        step = len(rest) - len(rest[1:].lstrip(" \t"))
        col += step
        rest = rest[step:]
    if ":" not in rest and dash is not None:
        return dash
    return col


def yaml_spans(text):
    spans = []
    block_parent = None
    block_indent = None
    quote = None
    for pos, line in iter_lines(text):
        stripped = line.lstrip(" ")
        indent = len(line) - len(stripped)
        if block_parent is not None:
            if stripped == "":
                continue
            if block_indent is None and indent > block_parent:
                block_indent = indent
                continue
            if block_indent is not None and indent >= block_indent:
                continue
            block_parent = None
            block_indent = None
        if quote is None and stripped.startswith("#"):
            spans.append((pos + indent, pos + len(line)))
            continue
        cut, quote = inline_comment(line, quote, yaml_quote_opener, "#")
        code = line if cut is None else line[:cut]
        if cut is not None:
            spans.append((pos + cut, pos + len(line)))
        if quote is None and YAML_BLOCK.search(code.rstrip()):
            block_parent = yaml_block_parent(code)
    return spans


def toml_spans(text):
    spans = []
    multiline = None
    for pos, line in iter_lines(text):
        k = 0
        n = len(line)
        quote = None
        cut = None
        while k < n:
            if multiline:
                if multiline == '"""' and line[k] == "\\":
                    k += 2
                    continue
                if line.startswith(multiline, k):
                    k += 3
                    while k < n and line[k] == multiline[0]:
                        k += 1
                    multiline = None
                    continue
                k += 1
                continue
            c = line[k]
            if quote:
                if c == "\\" and quote == '"':
                    k += 2
                    continue
                if c == quote:
                    quote = None
                k += 1
                continue
            if line.startswith('"""', k) or line.startswith("'''", k):
                multiline = line[k:k + 3]
                k += 3
                continue
            if c in "'\"":
                quote = c
                k += 1
                continue
            if c == "#":
                cut = k
                break
            k += 1
        if cut is not None:
            spans.append((pos + cut, pos + n))
    return spans


def env_spans(text):
    spans = []
    for pos, line in iter_lines(text):
        stripped = line.lstrip()
        if stripped.startswith("#"):
            spans.append((pos + len(line) - len(stripped), pos + len(line)))
            continue
        cut, _ = inline_comment(line, None, env_quote_opener, "#")
        if cut is not None:
            spans.append((pos + cut, pos + len(line)))
    return spans


def gitconfig_spans(text):
    spans = []
    for pos, line in iter_lines(text):
        stripped = line.lstrip()
        if stripped.startswith(("#", ";")):
            spans.append((pos + len(line) - len(stripped), pos + len(line)))
            continue
        cut, _ = inline_comment(line, None, gitconfig_quote_opener, "#;")
        if cut is not None:
            spans.append((pos + cut, pos + len(line)))
    return spans


def dockerfile_spans(text):
    spans = []
    in_directives = True
    heredocs = []
    for pos, line in iter_lines(text):
        if heredocs:
            strip_tabs, delim = heredocs[0]
            body = line.lstrip("\t") if strip_tabs else line
            if body == delim:
                heredocs.pop(0)
            continue
        stripped = line.lstrip()
        if in_directives and DOCKER_DIRECTIVE.match(stripped) and stripped == line:
            continue
        in_directives = False
        if stripped.startswith("#"):
            spans.append((pos + len(line) - len(stripped), pos + len(line)))
            continue
        heredocs.extend((m.group(1) == "-", m.group(2)) for m in DOCKER_HEREDOC.finditer(line))
    if heredocs:
        raise ScanError(f"unterminated heredoc {heredocs[0][1]}")
    return spans


def spans_for(language, text):
    if language == "cpp":
        return c_family_spans(text, proto=False)
    if language in ("proto", "json"):
        return c_family_spans(text, proto=True)
    if language == "cmake":
        return cmake_spans(text)
    if language == "shell":
        return shell_spans(text)
    if language == "python":
        return python_spans(text)
    if language == "sql":
        return sql_spans(text)
    if language == "dockerfile":
        return dockerfile_spans(text)
    if language == "yaml":
        return yaml_spans(text)
    if language == "toml":
        return toml_spans(text)
    if language == "env":
        return env_spans(text)
    if language == "gitconfig":
        return gitconfig_spans(text)
    if language == "hash":
        return full_line_spans(text, ("#",))
    raise ScanError(f"no scanner for {language}")


def classify(rel):
    path = Path(rel)
    name = path.name
    if rel not in FIRST_PARTY_UNDER_EXCLUDED:
        if rel.startswith(EXCLUDED_PREFIXES) or any(part in "/" + rel for part in EXCLUDED_PARTS):
            return None
    if name in NOT_CODE_NAMES or path.suffix in NOT_CODE_SUFFIXES:
        return None
    if name == "CMakeLists.txt" or path.suffix == ".cmake":
        return "cmake"
    if path.suffix in (".hxx", ".cc"):
        return "cpp"
    if path.suffix == ".proto":
        return "proto"
    if path.suffix == ".json":
        return "json"
    if path.suffix == ".sh":
        return "shell"
    if path.suffix == ".py":
        return "python"
    if path.suffix == ".sql":
        return "sql"
    if name == ".gitmodules":
        return "gitconfig"
    if name == ".env.example":
        return "env"
    if name in HASH_NAMES or name.endswith(".dockerignore"):
        return "hash"
    if name == "Dockerfile" or name.startswith("Dockerfile."):
        return "dockerfile"
    if name.endswith((".toml", ".toml.example")):
        return "toml"
    if path.suffix in (".yml", ".yaml") or name in (".clang-tidy", ".clang-format"):
        return "yaml"
    return "unknown"


def word_of(line):
    parts = line.strip().split()
    return parts[0].rstrip(";") if parts else ""


def ends_with_opener(line, language):
    stripped = line.rstrip()
    if not stripped:
        return False
    openers = OPENERS.get(language, ())
    last = stripped.split()[-1]
    return stripped.endswith(tuple(o for o in openers if len(o) == 1)) or last in openers


def starts_with_closer(line, language):
    stripped = line.strip()
    if not stripped:
        return False
    closers = CLOSERS.get(language, ())
    if language == "cmake" and re.match(r"(end\w*|else\w*)\s*\(", stripped):
        return True
    return stripped[0] in "".join(c for c in closers if len(c) == 1) or word_of(stripped) in closers


def join_split_spans(text, language):
    for start, end in sorted(spans_for(language, text), reverse=True):
        if "\n" not in text[start:end]:
            continue
        line_start = text.rfind("\n", 0, start) + 1
        line_end = end_of_line(text, end)
        if text[line_start:start].strip() and text[end:line_end].strip():
            text = text[:start].rstrip(" \t") + " " + text[end:].lstrip(" \t")
    return text


def strip_line(line, mask, pos):
    out = []
    k = 0
    trailing = False
    while k < len(line):
        if not mask[pos + k]:
            out.append(line[k])
            k += 1
            continue
        e = k
        while e < len(line) and mask[pos + e]:
            e += 1
        before = out[-1] if out else ""
        after = line[e] if e < len(line) else ""
        if e == len(line):
            trailing = True
        elif before in ("", " ", "\t") or after in (" ", "\t"):
            if before in ("", " ", "\t"):
                while e < len(line) and line[e] in " \t":
                    e += 1
        elif before not in UNSPACED_NEIGHBOURS and after not in UNSPACED_NEIGHBOURS:
            out.append(" ")
        k = e
    result = "".join(out)
    return result.rstrip() if trailing else result


def strip_spans(text, language):
    text = join_split_spans(text, language)
    spans = spans_for(language, text)
    mask = bytearray(len(text))
    for start, end in spans:
        for k in range(start, end):
            mask[k] = 1
    kept = []
    raws = text.split("\n")
    pos = 0
    for raw in raws:
        cr = raw.endswith("\r")
        line = raw[:-1] if cr else raw
        touched = any(mask[pos:pos + len(line)])
        pos_line = pos
        pos += len(raw) + 1
        if not touched:
            kept.append((line, cr, False))
            continue
        result = strip_line(line, mask, pos_line)
        if result.strip() == "":
            kept.append((None, cr, True))
        else:
            kept.append((result, cr, False))
    lines = []
    gap = False
    for line, cr, dropped in kept:
        if dropped:
            gap = True
            continue
        if gap:
            prev = lines[-1][0] if lines else None
            if line.strip() == "":
                if prev is None or prev.strip() == "" or ends_with_opener(prev, language):
                    continue
            elif prev is not None and prev.strip() == "" and starts_with_closer(line, language):
                while lines and lines[-1][0].strip() == "":
                    lines.pop()
            gap = False
        lines.append((line, cr))
    trailing_newline = text.endswith("\n")
    while lines and lines[-1][0].strip() == "":
        lines.pop()
    body = "\n".join(line + ("\r" if cr else "") for line, cr in lines)
    if not body:
        return ""
    return body + ("\r\n" if trailing_newline and lines[-1][1] else "\n")


def tracked_files(root):
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
            check=True, capture_output=True).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        raise ScanError(f"{root} is not a git work tree ({error})") from error
    return sorted({p for p in out.decode("utf-8").split("\0") if p})


def main():
    parser = argparse.ArgumentParser(prog="check-comments")
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument("--fix", action="store_true")
    parser.add_argument("paths", nargs="*")
    args = parser.parse_args()
    root = Path(args.root).resolve()
    try:
        candidates = args.paths or tracked_files(root)
    except ScanError as error:
        print(f"check-comments: {error}", file=sys.stderr)
        return 2
    checked = 0
    found = 0
    fixed = 0
    unknown = []
    failures = []
    for rel in candidates:
        path = root / rel
        if not path.is_file() or path.is_symlink():
            continue
        language = classify(rel)
        if language is None:
            continue
        if language == "unknown":
            unknown.append(rel)
            continue
        try:
            text = path.read_bytes().decode("utf-8")
        except UnicodeDecodeError:
            failures.append(f"{rel}: not UTF-8 text")
            continue
        try:
            spans = spans_for(language, text)
            if spans and args.fix:
                path.write_bytes(strip_spans(text, language).encode("utf-8"))
        except ScanError as error:
            failures.append(f"{rel}: {error}")
            continue
        checked += 1
        if not spans:
            continue
        found += len(spans)
        if args.fix:
            fixed += 1
            continue
        for start, end in spans:
            snippet = text[start:end].split("\n")[0][:72]
            print(f"comment: {rel}:{line_of(text, start)}: {snippet}")
    for rel in unknown:
        print(f"unclassified: {rel} has no comment scanner; classify it in scripts/lib/comment_scan.py")
    for failure in failures:
        print(f"unread: {failure}")
    if checked == 0:
        print("check-comments: nothing was checked", file=sys.stderr)
        return 2
    if args.fix:
        print(f"check-comments: {checked} files checked, {found} comments removed from {fixed} files")
    else:
        print(f"check-comments: {checked} files checked, {found} comments")
    if unknown or failures or (found and not args.fix):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
