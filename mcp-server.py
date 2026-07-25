#!/usr/bin/env python3
"""Argus MCP Server — on-demand codebase context for AI assistants.

Provides focused, minimal-token responses about the codebase via MCP.
Run with:

    .venv/bin/python3 mcp-server.py

Then configure in opencode.json:

    "mcp": {
      "argus-context": {
        "type": "local",
        "command": [".venv/bin/python3", "mcp-server.py"],
        "enabled": true
      }
    }

Resource URIs:
  argus://overview        - project overview (1 query)
  argus://symbols         - full compact symbol index (1 query)
  argus://file/{path}     - compact context for one file
  argus://class/{name}    - full class/method signatures

Tools:
  search_symbols            - find symbols by name
  get_class_info            - class details (methods, members, access)
  get_function_info         - function signatures across codebase
  get_dependencies          - include graph for a file (flat)
  get_file_symbols          - all symbols in a file
  get_project_stats         - code statistics
  find_callers              - find call sites of a function
  find_symbol_occurrences   - text-grep for a symbol name
  read_lines                - read specific lines from a file
  search_comments           - search C++ comments for a pattern
  find_todo                 - list TODO/FIXME/HACK markers
  get_includes_tree         - recursive include dependency tree
"""

from __future__ import annotations

import os
import re
import sys
import urllib.parse
from collections import deque
from pathlib import Path
from typing import Any

import tree_sitter
import tree_sitter_cpp as tscpp
from mcp.server.fastmcp import FastMCP

# ── paths ───────────────────────────────────────────────────────────────────
REPO_ROOT = Path(__file__).resolve().parent
SRC = REPO_ROOT / "src"


# ── tree-sitter setup (lazy) ───────────────────────────────────────────────
_LANG: tree_sitter.Language | None = None
_PARSER: tree_sitter.Parser | None = None


def _ensure_parser():
    global _LANG, _PARSER
    if _PARSER is None:
        _LANG = tree_sitter.Language(tscpp.language())
        _PARSER = tree_sitter.Parser(_LANG)


def _parse_file(path: Path) -> tree_sitter.Tree | None:
    _ensure_parser()
    try:
        with open(path, "rb") as f:
            return _PARSER.parse(f.read())
    except Exception:
        return None


# ── AST helpers (minimal, shared) ──────────────────────────────────────────

def _text(node) -> str:
    return node.text.decode("utf-8", errors="replace") if node.text else ""


def _param_list_str(node) -> str:
    params = []
    for child in node.named_children:
        if child.type == "parameter_declaration":
            parts = []
            for c in child.children:
                if c.type == "function_declarator":
                    parts.append(__decl_to_str(c))
                else:
                    t = _text(c).strip()
                    if t:
                        parts.append(t)
            params.append(" ".join(parts))
    return "(" + ", ".join(params) + ")"


def __decl_to_str(node) -> str:
    if node.type in ("identifier", "field_identifier"):
        return _text(node)
    if node.type == "function_declarator":
        name, args, quals = "", "", ""
        for c in node.children:
            if c.type in ("identifier", "field_identifier", "qualified_identifier", "template_function_declarator"):
                name = _text(c)
            elif c.type == "parameter_list":
                args = _param_list_str(c)
            elif c.type == "type_qualifier":
                q = _text(c).strip()
                if q:
                    quals += " " + q
            else:
                t = _text(c).strip()
                if t and t not in ("(", ")"):
                    name += t
        return f"{name}{args}{quals}"
    if node.type == "pointer_declarator":
        return "*" + (__decl_to_str(node.children[1]) if node.child_count > 1 else "")
    if node.type == "reference_declarator":
        return "&" + (__decl_to_str(node.children[1]) if node.child_count > 1 else "")
    if node.type == "parenthesized_declarator":
        return "(" + (__decl_to_str(node.children[1]) if node.child_count > 1 else "") + ")"
    return _text(node)


def _collect_type_prefix(node) -> list[str]:
    parts = []
    for c in node.children:
        if c.type in (
            "primitive_type", "type_identifier", "qualified_identifier",
            "template_type", "sized_type_specifier", "type_qualifier",
            "virtual", "static",
        ):
            t = _text(c).strip()
            if t:
                parts.append(t)
        if c.type == "pointer_declarator" and c.child_count > 0 and _text(c.children[0]).strip() == "*":
            parts.append("*")
    return parts


def _sig_from_decl(decl_node, return_type_parts) -> str:
    for c in decl_node.children:
        if c.type in ("function_declarator", "function_definition"):
            decl_str = __decl_to_str(c)
            sig_parts = return_type_parts + [decl_str]
            nxt = c.next_sibling
            trailer = ""
            while nxt and nxt.type not in ("compound_statement", "field_initializer_list", ",", ")", ";", "{"):
                t = _text(nxt).strip()
                if t:
                    trailer += " " + t
                nxt = nxt.next_sibling
            return " ".join(p for p in sig_parts if p) + trailer
    return ""


def _is_func_decl(node) -> bool:
    if node.type != "declaration":
        return False
    return any(c.type in ("function_declarator", "function_definition") for c in node.children)


def _extract_class(node) -> dict | None:
    # Skip forward declarations (no body)
    if not node.child_by_field_name("body"):
        return None
    cls = {}
    cls["kind"] = "struct" if node.type == "struct_specifier" else "class"
    name_node = node.child_by_field_name("name")
    cls["name"] = _text(name_node) if name_node else "(anonymous)"
    bases_node = node.child_by_field_name("bases")
    cls["bases"] = ""
    if bases_node:
        bases = [_text(b) for b in bases_node.named_children]
        if bases:
            cls["bases"] = " : " + ", ".join(bases)
    cls["methods"] = []
    cls["members"] = []
    current_access = "public"
    body = node.child_by_field_name("body")
    for c in body.children:
        if c.type == "access_specifier":
            for t in c.children:
                if _text(t).strip() in ("public", "private", "protected"):
                    current_access = _text(t).strip()
        elif c.type == "field_declaration":
            has_func = any(x.type in ("function_declarator", "function_definition") for x in c.children)
            has_field_id = any(x.type == "field_identifier" for x in c.children)
            if has_func:
                rt = _collect_type_prefix(c)
                sig = _sig_from_decl(c, rt)
                if sig:
                    cls["methods"].append((current_access, sig))
            elif has_field_id:
                cls["members"].append((current_access, _text(c).strip().rstrip(";").strip()))
        elif c.type == "declaration" and _is_func_decl(c):
            rt = _collect_type_prefix(c)
            sig = _sig_from_decl(c, rt)
            if sig:
                cls["methods"].append((current_access, sig))
    return cls


def _extract_enum(node) -> str | None:
    if node.type != "enum_specifier":
        return None
    name_node = node.child_by_field_name("name")
    name = _text(name_node) if name_node else "(anonymous)"
    body = node.child_by_field_name("body")
    items = []
    if body:
        for c in body.named_children:
            if c.type == "enumerator":
                items.append(_text(c.child_by_field_name("name")))
    if items:
        return f"enum {name} {{ " + ", ".join(items) + " }"
    return f"enum {name}"


def _walk_file(tree: tree_sitter.Tree) -> dict:
    """Extract all symbols from a parsed file."""
    root = tree.root_node
    info = {
        "includes": [],
        "classes": [],
        "enums": [],
        "functions": [],
        "declarations": [],
    }

    def walk(node, depth=0):
        if depth > 50:
            return
        if node.type == "preproc_include":
            pn = node.child_by_field_name("path")
            if pn:
                info["includes"].append(_text(pn))
            return
        if node.type in ("class_specifier", "struct_specifier"):
            cls = _extract_class(node)
            if cls:
                info["classes"].append(cls)
            return
        if node.type == "enum_specifier":
            e = _extract_enum(node)
            if e:
                info["enums"].append(e)
            return
        if node.type == "function_definition":
            rt = _collect_type_prefix(node)
            sig = ""
            for c in node.children:
                if c.type == "function_declarator":
                    sig = " ".join(rt + [__decl_to_str(c)])
                    break
            if sig:
                info["functions"].append(sig)
            return
        if node.type == "declaration" and _is_func_decl(node):
            rt = _collect_type_prefix(node)
            sig = _sig_from_decl(node, rt)
            if sig:
                info["declarations"].append(sig)
            return
        if node.type == "template_declaration":
            for c in node.named_children:
                if c.type in ("class_specifier", "struct_specifier"):
                    cls = _extract_class(c)
                    if cls:
                        cls["template"] = True
                        info["classes"].append(cls)
                    return
                if c.type == "function_definition":
                    rt = _collect_type_prefix(c)
                    for cc in c.children:
                        if cc.type == "function_declarator":
                            sig = " ".join(["template"] + rt + [__decl_to_str(cc)])
                            info["functions"].append(sig)
                            return
                if c.type == "declaration" and _is_func_decl(c):
                    rt = _collect_type_prefix(c)
                    sig = _sig_from_decl(c, rt)
                    if sig:
                        info["declarations"].append("template " + sig)
                    return
            return
        for c in node.children:
            walk(c, depth + 1)

    for c in root.children:
        walk(c)
    return info


# ── codebase index (built once at startup) ──────────────────────────────────

class CodebaseIndex:
    """Pre-parsed index of all source files."""

    def __init__(self, src_dir: Path):
        self.src_dir = src_dir
        self.files: dict[str, dict] = {}  # rel_path -> file info
        self.classes: dict[str, list[dict]] = {}  # class name -> list of classes
        self.functions: dict[str, list[str]] = {}  # func_name -> list of signatures
        self.all_symbols: list[dict] = []  # for search

        self._build()

    def _build(self):
        sources = sorted(self.src_dir.rglob("*.cc")) + sorted(self.src_dir.rglob("*.hxx"))
        for fpath in sources:
            tree = _parse_file(fpath)
            if tree is None:
                continue
            rel = str(fpath.relative_to(REPO_ROOT))
            info = _walk_file(tree)
            info["path"] = rel
            self.files[rel] = info

            for cls in info["classes"]:
                name = cls["name"]
                cls["file"] = rel
                self.classes.setdefault(name, []).append(cls)
                self.all_symbols.append({
                    "type": cls["kind"],
                    "name": name,
                    "file": rel,
                    "detail": f"{cls['kind']} {name}{cls['bases']}",
                })
                for acc, sig in cls["methods"]:
                    mname = sig.split("(")[0].split()[-1].split("::")[-1]
                    self.all_symbols.append({
                        "type": "method",
                        "name": f"{name}::{mname}",
                        "file": rel,
                        "detail": f"{acc}: {sig}",
                    })

            for sig in info["functions"]:
                name = sig.split("(")[0].split()[-1].split("::")[-1]
                self.functions.setdefault(name, []).append(sig)
                self.all_symbols.append({
                    "type": "function",
                    "name": name,
                    "file": rel,
                    "detail": sig,
                })

            for sig in info["declarations"]:
                name = sig.split("(")[0].split()[-1].split("::")[-1]
                self.functions.setdefault(name, []).append(sig)
                self.all_symbols.append({
                    "type": "declaration",
                    "name": name,
                    "file": rel,
                    "detail": sig,
                })

            for enum in info["enums"]:
                ename = enum.split("{")[0].replace("enum", "").strip()
                self.all_symbols.append({
                    "type": "enum",
                    "name": ename,
                    "file": rel,
                    "detail": enum,
                })

    def search(self, query: str, limit: int = 10) -> list[dict]:
        q = query.lower()
        results = []
        for sym in self.all_symbols:
            if q in sym["name"].lower() or q in sym["detail"].lower():
                results.append(sym)
                if len(results) >= limit:
                    break
        return results

    def file_context(self, path: str) -> str | None:
        info = self.files.get(path)
        if not info:
            return None
        lines = [f"## `{info['path']}`"]
        if info["includes"]:
            project_includes = [i for i in info["includes"] if not i.startswith("<")]
            if project_includes:
                lines.append(f"local deps: {', '.join(project_includes)}")
        for cls in info["classes"]:
            t = cls.get("kind", "class")
            tag = "template " if cls.get("template") else ""
            lines.append(f"\n{tag}{t} {cls['name']}{cls['bases']}")
            for acc, sig in cls["methods"]:
                lines.append(f"  {acc}: `{sig}`")
            for acc, mem in cls["members"]:
                lines.append(f"  {acc}: {mem}")
        for sig in info["functions"]:
            lines.append(f"\n{sig}")
        for sig in info["declarations"]:
            lines.append(f"\n{sig}")
        for enum in info["enums"]:
            lines.append(f"\n{enum}")
        return "\n".join(lines)

    def class_info(self, name: str) -> str | None:
        classes = self.classes.get(name)
        if not classes:
            return None
        lines = []
        for cls in classes:
            t = cls.get("kind", "class")
            tag = "template " if cls.get("template") else ""
            lines.append(f"**{tag}{t} `{cls['name']}`**{cls['bases']}")
            lines.append(f"  _(`{cls.get('file', '?')}`)_\n")
            by_acc = {"public": [], "private": [], "protected": []}
            for acc, sig in cls["methods"]:
                by_acc.setdefault(acc, []).append(sig)
            for acc in ("public", "private", "protected"):
                if by_acc[acc]:
                    lines.append(f"  {acc}:")
                    for sig in by_acc[acc]:
                        lines.append(f"    - `{sig}`")
            for acc, mem in cls["members"]:
                lines.append(f"  {acc} member: `{mem}`")
        return "\n".join(lines)

    def find_occurrences(self, name: str, limit: int = 20) -> list[dict]:
        """Text-grep for a symbol name across all source files."""
        results = []
        sources = sorted(self.src_dir.rglob("*.cc")) + sorted(self.src_dir.rglob("*.hxx"))
        for fpath in sources:
            if len(results) >= limit:
                break
            try:
                with open(fpath, "r", encoding="utf-8", errors="replace") as f:
                    for lineno, line in enumerate(f, 1):
                        if name in line:
                            rel = str(fpath.relative_to(REPO_ROOT))
                            results.append({
                                "file": rel,
                                "line": lineno,
                                "text": line.strip()[:120],
                            })
                            if len(results) >= limit:
                                break
            except Exception:
                pass
        return results

    def find_callers(self, name: str, limit: int = 20) -> list[dict]:
        """Find call sites: lines containing 'name(' outside the definition itself."""
        raw = self.find_occurrences(name, 500)
        results = []
        pattern = re.compile(
            r'\b' + re.escape(name) + r'\s*\('
        )
        for occ in raw:
            if pattern.search(occ["text"]):
                if len(results) >= limit:
                    break
                results.append(occ)
        return results

    def search_comments(self, pattern: str, limit: int = 20) -> list[dict]:
        """Search C++ comments across all source files."""
        results = []
        sources = sorted(self.src_dir.rglob("*.cc")) + sorted(self.src_dir.rglob("*.hxx"))
        for fpath in sources:
            if len(results) >= limit:
                break
            try:
                with open(fpath, "r", encoding="utf-8", errors="replace") as f:
                    content = f.read()
                matches = re.finditer(
                    r'(?://(.*?)$|/\*(.*?)\*/)',
                    content,
                    re.MULTILINE | re.DOTALL,
                )
                for m in matches:
                    comment = (m.group(1) or m.group(2) or "").strip()
                    if re.search(pattern, comment, re.IGNORECASE):
                        lineno = content[: m.start()].count("\n") + 1
                        rel = str(fpath.relative_to(REPO_ROOT))
                        results.append({
                            "file": rel,
                            "line": lineno,
                            "text": comment[:120],
                        })
                        if len(results) >= limit:
                            break
            except Exception:
                pass
        return results

    def includes_tree(self, path: str, max_depth: int = 3) -> str:
        """Recursive include graph starting from a file."""
        seen = {path}
        lines = []

        def _resolve_include(raw_inc: str) -> str | None:
            """Map an include string to a file path in the index."""
            clean = raw_inc.strip('"').strip("<>")
            if clean in self.files:
                return clean
            # Try as relative to SRC
            for fp in self.files:
                if fp.endswith(clean) or fp == f"src/{clean}":
                    return fp
            return None

        def _recurse(p: str, depth: int, prefix: str):
            if depth > max_depth:
                return
            info = self.files.get(p)
            if not info:
                if depth > 0:
                    lines.append(f"{prefix}─ `{p}` (not indexed)")
                return
            if depth == 0:
                lines.append(f"`{p}`")
            for raw in info.get("includes", []):
                resolved = _resolve_include(raw)
                if not resolved:
                    continue
                if resolved in seen:
                    lines.append(f"{prefix}─ `{resolved}` (already shown)")
                    continue
                seen.add(resolved)
                if len(seen) > 80:
                    lines.append(f"{prefix}─ ... (too many)")
                    return
                lines.append(f"{prefix}─ `{resolved}`")
                _recurse(resolved, depth + 1, prefix + "  ")

        _recurse(path, 0, "  ")
        return "\n".join(lines) if len(lines) > 1 else "\n".join(lines) + "\n(no local includes found)"


# ── file-level helpers ────────────────────────────────────────────────────

def _read_file_lines(path: str, start: int, count: int) -> str | None:
    """Read count lines from a file starting at start_line (1-based)."""
    fpath = (REPO_ROOT / path).resolve()
    if not fpath.is_file() or not str(fpath).startswith(str(REPO_ROOT)):
        return None
    try:
        with open(fpath, "r", encoding="utf-8", errors="replace") as f:
            all_lines = f.readlines()
        end = min(start + count - 1, len(all_lines))
        if start > len(all_lines):
            return f"File has only {len(all_lines)} lines."
        result = []
        for i in range(start - 1, end):
            result.append(f"{i + 1}: {all_lines[i].rstrip()}")
        return "\n".join(result)
    except Exception as e:
        return f"Error: {e}"


# ── build index ────────────────────────────────────────────────────────────
index: CodebaseIndex | None = None


def _get_index() -> CodebaseIndex:
    global index
    if index is None:
        index = CodebaseIndex(SRC)
    return index


# ── MCP server ─────────────────────────────────────────────────────────────

mcp = FastMCP(
    "Argus Codebase Context",
    instructions=(
        "MCP server for the Argus backend C++20 codebase (Drogon, SQLite, ONNX TTS). "
        "12 tools + 4 resources for structured code queries. "
        "All responses return only relevant symbols — no full file dumps, "
        "keeping token usage minimal (~80-95% less vs raw source)."
    ),
)


# ── resources ──────────────────────────────────────────────────────────────

@mcp.resource("argus://overview", description="Project overview: files, languages, structure")
def overview() -> str:
    idx = _get_index()
    ext_cc = sum(1 for p in idx.files if p.endswith(".cc"))
    ext_hxx = sum(1 for p in idx.files if p.endswith(".hxx"))
    class_count = len(idx.classes)
    func_count = len(idx.functions)
    return (
        "# Argus Backend\n\n"
        f"- **Language:** C++20 (Drogon framework)\n"
        f"- **Source files:** {ext_cc + ext_hxx} ({ext_cc} `.cc`, {ext_hxx} `.hxx`)\n"
        f"- **Classes:** {class_count}\n"
        f"- **Functions:** {func_count}\n"
        f"- **Symbols indexed:** {len(idx.all_symbols)}\n"
        f"- **MCP resources:** `argus://symbols`, `argus://file/{{path}}`, `argus://class/{{name}}`\n"
        f"- **MCP tools (12):** `search_symbols`, `get_class_info`, `get_function_info`, "
        f"`get_dependencies`, `get_file_symbols`, `get_project_stats`, "
        f"`find_callers`, `find_symbol_occurrences`, `read_lines`, "
        f"`search_comments`, `find_todo`, `get_includes_tree`\n"
    )


@mcp.resource("argus://symbols", description="Full compact symbol index")
def all_symbols() -> str:
    idx = _get_index()
    lines = ["# Argus Backend — Symbol Index\n"]
    for rel, info in sorted(idx.files.items()):
        lines.append(f"\n## `{rel}`")
        project_inc = [i for i in info["includes"] if not i.startswith("<")]
        if project_inc:
            lines.append(f"local deps: {', '.join(project_inc)}")
        for cls in info["classes"]:
            tag = "template " if cls.get("template") else ""
            lines.append(f"\n**{tag}{cls['kind']} {cls['name']}**{cls['bases']}")
            for acc, sig in cls["methods"]:
                lines.append(f"  {acc}: `{sig}`")
            for acc, mem in cls["members"]:
                lines.append(f"  {acc}: `{mem}`")
        for sig in info["functions"]:
            lines.append(f"\n`{sig}`")
        for sig in info["declarations"]:
            lines.append(f"\n`{sig}`")
        for enum in info["enums"]:
            lines.append(f"\n{enum}")
    return "\n".join(lines)


@mcp.resource("argus://file/{path}", description="Compact context for one source file (path relative to repo root, URL-encoded if multi-segment, e.g. 'src%2Fmain.cc'). Use `get_file_symbols` tool for convenience.")
def file_resource(path: str) -> str:
    path = urllib.parse.unquote(path)
    ctx = _get_index().file_context(path)
    if ctx is None:
        return f"File not found: {path}\n\nAvailable files:\n" + "\n".join(
            f"  {p}" for p in sorted(_get_index().files.keys())
        )
    return ctx


@mcp.resource("argus://class/{name}", description="Full class/method signatures by class name")
def class_resource(name: str) -> str:
    name = urllib.parse.unquote(name)
    info = _get_index().class_info(name)
    if info is None:
        return f"Class '{name}' not found.\n\nAvailable classes: {', '.join(sorted(_get_index().classes.keys()))}"
    return info


# ── tools ──────────────────────────────────────────────────────────────────

@mcp.tool(description="Search symbols by name or pattern. Returns type, name, file, and detail.")
def search_symbols(query: str, limit: int = 10) -> str:
    results = _get_index().search(query, limit)
    if not results:
        return "No symbols found."
    lines = [f"### Search results for '{query}' ({len(results)} shown)\n"]
    for r in results:
        lines.append(f"- **{r['type']}** `{r['name']}` — `{r['file']}`")
        lines.append(f"  {r['detail']}")
    return "\n".join(lines)


@mcp.tool(description="Get full class details: methods, members, access specifiers, bases.")
def get_class_info(name: str) -> str:
    info = _get_index().class_info(name)
    if info is None:
        return f"Class '{name}' not found.\n\nAvailable classes: {', '.join(sorted(_get_index().classes.keys()))}"
    return info


@mcp.tool(description="Find function signatures by name across the codebase.")
def get_function_info(name: str) -> str:
    sigs = _get_index().functions.get(name)
    if not sigs:
        return f"Function '{name}' not found."
    lines = [f"### Function `{name}`"]
    for sig in sigs:
        lines.append(f"- `{sig}`")
    return "\n".join(lines)


@mcp.tool(description="Get include/dependency graph for a source file (path relative to repo root).")
def get_dependencies(path: str) -> str:
    info = _get_index().files.get(path)
    if not info:
        return f"File not found: {path}"
    lines = [f"### Dependencies for `{path}`\n"]
    sys_inc = [i for i in info["includes"] if i.startswith("<")]
    proj_inc = [i for i in info["includes"] if not i.startswith("<")]
    if proj_inc:
        lines.append("**Local:**")
        for i in proj_inc:
            lines.append(f"- `{i}`")
    if sys_inc:
        lines.append("\n**System:**")
        for i in sys_inc:
            lines.append(f"- `{i}`")
    return "\n".join(lines)


@mcp.tool(description="Get all symbols (classes, functions, enums) in a source file.")
def get_file_symbols(path: str) -> str:
    ctx = _get_index().file_context(path)
    if ctx is None:
        return f"File not found: {path}"
    return ctx


@mcp.tool(description="Get codebase statistics: file counts, class/function counts, LOC.")
def get_project_stats() -> str:
    idx = _get_index()
    cc = sum(1 for p in idx.files if p.endswith(".cc"))
    hxx = sum(1 for p in idx.files if p.endswith(".hxx"))
    total_files = cc + hxx

    class_count = len(idx.classes)
    func_count = len(idx.functions)
    method_count = sum(len(cls["methods"]) for cls_list in idx.classes.values() for cls in cls_list)
    member_count = sum(len(cls["members"]) for cls_list in idx.classes.values() for cls in cls_list)

    return (
        f"**Project Stats — Argus Backend**\n\n"
        f"- Source files: {total_files} ({cc} `.cc`, {hxx} `.hxx`)\n"
        f"- Classes/Structs: {class_count}\n"
        f"- Methods: {method_count}\n"
        f"- Members: {member_count}\n"
        f"- Functions (free): {func_count}\n"
        f"- Total symbols indexed: {len(idx.all_symbols)}\n"
    )


@mcp.tool(description="Find call sites where a function is invoked. Searches for 'name(' pattern across all source files.")
def find_callers(name: str, limit: int = 15) -> str:
    idx = _get_index()
    results = idx.find_callers(name, limit)
    if not results:
        return f"No call sites found for '{name}'."
    lines = [f"### Callers of `{name}` ({len(results)} shown)\n"]
    for r in results:
        lines.append(f"- `{r['file']}:{r['line']}`  {r['text']}")
    return "\n".join(lines)


@mcp.tool(description="Find all occurrences of a symbol name across all source files (text-grep).")
def find_symbol_occurrences(name: str, limit: int = 20) -> str:
    idx = _get_index()
    results = idx.find_occurrences(name, limit)
    if not results:
        return f"No occurrences of '{name}'."
    lines = [f"### Occurrences of `{name}` ({len(results)} shown)\n"]
    for r in results:
        lines.append(f"- `{r['file']}:{r['line']}`  {r['text']}")
    return "\n".join(lines)


@mcp.tool(description="Read specific lines from a source file. Path relative to repo root. start_line is 1-based.")
def read_lines(path: str, start_line: int, count: int = 20) -> str:
    result = _read_file_lines(path, start_line, count)
    if result is None:
        return f"File not found or outside repo: {path}"
    return result


@mcp.tool(description="Search C++ comments (both // and /* */) across all source files for a pattern.")
def search_comments(pattern: str, limit: int = 15) -> str:
    idx = _get_index()
    results = idx.search_comments(pattern, limit)
    if not results:
        return f"No comments matching '{pattern}'."
    lines = [f"### Comments matching '{pattern}' ({len(results)} shown)\n"]
    for r in results:
        lines.append(f"- `{r['file']}:{r['line']}`  {r['text']}")
    return "\n".join(lines)


@mcp.tool(description="Find TODO, FIXME, HACK, TEMP markers in code comments.")
def find_todo() -> str:
    idx = _get_index()
    results = idx.search_comments(r"TODO|FIXME|HACK|TEMP", 30)
    if not results:
        return "No TODO/FIXME/HACK/TEMP markers found."
    lines = ["### TODOs and FIXMEs\n"]
    for r in results:
        label = ""
        for tag in ("TODO", "FIXME", "HACK", "TEMP"):
            if tag in r["text"].upper():
                label = tag
                break
        lines.append(f"- **{label}** `{r['file']}:{r['line']}`  {r['text']}")
    return "\n".join(lines)


@mcp.tool(description="Build recursive include tree for a source file (local includes only, up to max_depth).")
def get_includes_tree(path: str, max_depth: int = 3) -> str:
    idx = _get_index()
    if path not in idx.files:
        return f"File not found: {path}"
    return f"### Include tree for `{path}`\n\n" + idx.includes_tree(path, min(max_depth, 5))


# ── main ───────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    mcp.run(transport="stdio")
