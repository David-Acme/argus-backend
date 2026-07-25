#!/usr/bin/env python3
"""dev-context.py — Tree-sitter codebase context extractor.

Generates a compact Markdown symbol index of the C++ codebase for AI
consumption. Run after any significant code change to refresh context:

    .venv/bin/python3 scripts/dev-context.py > CONTEXT_SYMBOLS.md

Output is ~5-15 KB instead of 100+ KB of raw source — 90%+ token savings
for AI code-assistance sessions.
"""

import argparse
import os
import sys
from pathlib import Path

import tree_sitter
import tree_sitter_cpp as tscpp

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC = REPO_ROOT / "src"

# ── tree-sitter setup ──────────────────────────────────────────────────────
LANG = tree_sitter.Language(tscpp.language())
PARSER = tree_sitter.Parser(LANG)


# ── helpers ─────────────────────────────────────────────────────────────────

def node_text(node) -> str:
    return node.text.decode("utf-8", errors="replace") if node.text else ""


def signature_of_function(node) -> str:
    """Return the signature of a function declaration/definition node.

    Handles:
      - `declaration` (prototype)     void foo(int a, float b);
      - `function_definition` (body)  void foo(int a, float b) { ... }
      - `field_declaration` (in class) void foo(int a, float b);
    """
    parts = []
    for child in node.children:
        if child.type == "function_definition":
            return signature_of_function(child)
        if child.type == "function_declarator":
            parts.append(node_text(child))
        elif child.type in (
            "primitive_type",
            "type_identifier",
            "qualified_identifier",
            "template_type",
            "sized_type_specifier",
        ):
            parts.append(node_text(child))
        elif child.type in (
            "pointer_declarator",
            "reference_declarator",
            "parenthesized_declarator",
        ):
            # The declarator holds the name, but also wraps pointers/references
            pass  
    sig = " ".join(p for p in parts if p.strip() and p.strip() not in (";", ""))
    return sig


def function_signature_from_declarator(decl_node, return_type_parts) -> str:
    sig_parts = list(return_type_parts)
    sig_parts.append(_declarator_to_str(decl_node))
    nxt = decl_node.next_sibling
    trailer = ""
    while nxt and nxt.type not in ("compound_statement", "field_initializer_list", ",", ")", ";", "{"):
        t = node_text(nxt)
        if t.strip():
            trailer += " " + t.strip()
        nxt = nxt.next_sibling
    return " ".join(p for p in sig_parts if p) + trailer


def _declarator_to_str(node) -> str:
    if node.type == "identifier":
        return node_text(node)
    if node.type == "field_identifier":
        return node_text(node)
    if node.type == "function_declarator":
        name = ""
        args = ""
        qualifiers = ""
        for child in node.children:
            if child.type == "identifier":
                name = node_text(child)
            elif child.type == "field_identifier":
                name = node_text(child)
            elif child.type == "parameter_list":
                args = _parameter_list_to_str(child)
            elif child.type == "qualified_identifier":
                name = node_text(child)
            elif child.type == "template_function_declarator":
                name = node_text(child)
            elif child.type == "type_qualifier":
                q = node_text(child).strip()
                if q:
                    qualifiers += " " + q
            else:
                t = node_text(child)
                if t.strip() and t.strip() not in ("(", ")"):
                    name += t.strip()
        return f"{name}{args}{qualifiers}"
    if node.type == "pointer_declarator":
        inner = _declarator_to_str(node.child(1)) if node.child_count > 1 else ""
        return f"*{inner}"
    if node.type == "reference_declarator":
        inner = _declarator_to_str(node.child(1)) if node.child_count > 1 else ""
        return f"&{inner}"
    if node.type == "parenthesized_declarator":
        inner = _declarator_to_str(node.child(1)) if node.child_count > 1 else ""
        return f"({inner})"
    if node.type == "array_declarator":
        inner = _declarator_to_str(node.child(0)) if node.child_count > 0 else ""
        return f"{inner}[]"
    return node_text(node)


def _parameter_list_to_str(node) -> str:
    params = []
    for child in node.named_children:
        if child.type == "parameter_declaration":
            params.append(_parameter_to_str(child))
    return "(" + ", ".join(params) + ")"


def _parameter_to_str(node) -> str:
    parts = []
    for child in node.children:
        if child.type == "function_declarator":
            parts.append(_declarator_to_str(child))
        else:
            t = node_text(child)
            if t.strip():
                parts.append(t.strip())
    return " ".join(parts)


def collect_type_parts(node) -> list[str]:
    """Walk children before a declarator to find return-type tokens."""
    parts = []
    for child in node.children:
        if child.type in (
            "primitive_type",
            "type_identifier",
            "qualified_identifier",
            "template_type",
            "sized_type_specifier",
            "type_qualifier",
            "virtual",
            "static",
            "virtual_specifier",
            "attribute_declaration",
        ):
            t = node_text(child)
            if t.strip():
                parts.append(t.strip())
        if child.type == "pointer_declarator":
            # pointer relative to type (e.g. `float*`)
            if child.child_count > 0:
                inner = node_text(child.child(0))
                if inner.strip() == "*":
                    parts.append("*")
    return parts


def is_function_declaration(node) -> bool:
    """True if node is a 'declaration' that declares a function (not a var)."""
    if node.type != "declaration":
        return False
    for child in node.children:
        if child.type in ("function_declarator", "function_definition"):
            return True
    return False


def extract_signature(node) -> str | None:
    """Extract signature from declaration or field_declaration nodes."""
    if node.type == "declaration":
        if not is_function_declaration(node):
            return None
        return_type = collect_type_parts(node)
        for child in node.children:
            if child.type in ("function_declarator", "function_definition"):
                return function_signature_from_declarator(child, return_type)
        return None

    if node.type == "field_declaration":
        # could be method or data member
        return_type = collect_type_parts(node)
        for child in node.children:
            if child.type in ("function_declarator", "function_definition"):
                return function_signature_from_declarator(child, return_type)
            if child.type == "field_identifier":
                return None  # data member, skip
        return None

    if node.type == "function_definition":
        return_type = collect_type_parts(node)
        for child in node.children:
            if child.type == "function_declarator":
                return function_signature_from_declarator(child, return_type)
        return None

    return None


def is_data_member(node) -> bool:
    if node.type != "field_declaration":
        return False
    for child in node.children:
        if child.type == "field_identifier":
            # has field_identifier but no function_declarator → data member
            has_func = any(
                c.type in ("function_declarator", "function_definition")
                for c in node.children
            )
            return not has_func
    return False


def extract_data_member(node) -> str | None:
    if not is_data_member(node):
        return None
    return node_text(node).strip().rstrip(";").strip()


def extract_class(node) -> dict:
    """Parse a class_specifier or struct_specifier node."""
    kind = node.type  # class_specifier or struct_specifier
    name_node = node.child_by_field_name("name")
    name = node_text(name_node) if name_node else "(anonymous)"

    bases = ""
    base_node = node.child_by_field_name("bases")
    if base_node:
        bases_list = []
        for base in base_node.named_children:
            bases_list.append(node_text(base))
        if bases_list:
            bases = " : " + ", ".join(bases_list)

    methods = []
    members = []
    current_access = "public"

    body = node.child_by_field_name("body")
    if not body:
        return {"kind": kind, "name": name, "bases": bases, "methods": methods, "members": members}

    for child in body.children:
        if child.type == "access_specifier":
            spec_text = node_text(child).strip()
            if spec_text in ("public", "private", "protected"):
                # Normalize: the token before ':' might be one of these
                for token in child.children:
                    t = node_text(token).strip()
                    if t in ("public", "private", "protected"):
                        current_access = t
                        break
        elif child.type == "field_declaration":
            sig = extract_signature(child)
            if sig:
                methods.append({"access": current_access, "signature": sig})
            else:
                dm = extract_data_member(child)
                if dm:
                    members.append({"access": current_access, "decl": dm})
        elif child.type == "declaration":
            if is_function_declaration(child):
                sig = extract_signature(child)
                if sig:
                    methods.append({"access": current_access, "signature": sig})

    return {
        "kind": "struct" if kind == "struct_specifier" else "class",
        "name": name,
        "bases": bases,
        "methods": methods,
        "members": members,
    }


def extract_enum(node) -> str | None:
    if node.type != "enum_specifier":
        return None
    name_node = node.child_by_field_name("name")
    name = node_text(name_node) if name_node else "(anonymous)"
    body = node.child_by_field_name("body")
    enumerators = []
    if body:
        for child in body.named_children:
            if child.type == "enumerator":
                enumerators.append(node_text(child.child_by_field_name("name")))
    if enumerators:
        return f"enum {name} {{ {' ,'.join(enumerators)} }}"
    return f"enum {name}"


def extract_namespace(node) -> str:
    name_node = node.child_by_field_name("name")
    return node_text(name_node) if name_node else "(anonymous)"


def extract_include(node) -> str | None:
    if node.type != "preproc_include":
        return None
    path_node = node.child_by_field_name("path")
    if path_node:
        return node_text(path_node)
    return node_text(node).strip()


# ── file-level extraction ──────────────────────────────────────────────────

def extract_file(filepath: Path) -> dict:
    with open(filepath, "rb") as f:
        code = f.read()

    tree = PARSER.parse(code)
    root = tree.root_node

    includes = []
    namespaces: list[str] = []
    classes = []
    enums = []
    free_functions = []
    free_declarations = []
    other_decls = []

    current_ns: list[str] = ["<global>"]
    ns_depth = 0

    def walk(node, depth=0):
        nonlocal current_ns, ns_depth

        if node.type == "preproc_include":
            inc = extract_include(node)
            if inc:
                includes.append(inc)
            return

        if node.type == "namespace_definition":
            ns_name = extract_namespace(node)
            current_ns.append(ns_name)
            ns_depth += 1
            body = node.child_by_field_name("body")
            if body:
                for child in body.children:
                    walk(child, depth + 1)
            ns_depth -= 1
            current_ns.pop()
            return

        if node.type == "class_specifier" or node.type == "struct_specifier":
            cls = extract_class(node)
            cls["namespace"] = list(current_ns)
            classes.append(cls)
            return

        if node.type == "enum_specifier":
            e = extract_enum(node)
            if e:
                enums.append({"namespace": list(current_ns), "decl": e})
            return

        if node.type == "function_definition":
            sig = extract_signature(node)
            if sig:
                free_functions.append({"namespace": list(current_ns), "signature": sig})
            return

        if node.type == "declaration" and is_function_declaration(node):
            sig = extract_signature(node)
            if sig:
                free_declarations.append({"namespace": list(current_ns), "signature": sig})
            return

        if node.type == "template_declaration":
            # peek at the child declaration
            for child in node.named_children:
                if child.type in ("class_specifier", "struct_specifier"):
                    cls = extract_class(child)
                    cls["namespace"] = list(current_ns)
                    cls["template"] = True
                    classes.append(cls)
                    return
                if child.type == "function_definition":
                    sig = extract_signature(child)
                    if sig:
                        free_functions.append(
                            {"namespace": list(current_ns), "signature": f"template {sig}", "template": True}
                        )
                    return
                if child.type == "declaration" and is_function_declaration(child):
                    sig = extract_signature(child)
                    if sig:
                        free_declarations.append(
                            {"namespace": list(current_ns), "signature": f"template {sig}", "template": True}
                        )
                    return
            return

        # Recurse into other container nodes
        for child in node.children:
            walk(child, depth + 1)

    for child in root.children:
        walk(child)

    return {
        "file": str(filepath.relative_to(REPO_ROOT)),
        "includes": includes,
        "classes": classes,
        "enums": enums,
        "free_functions": free_functions,
        "free_declarations": free_declarations,
    }


# ── output ──────────────────────────────────────────────────────────────────

def format_markdown(data: list[dict]) -> str:
    lines: list[str] = []

    lines.append("# Argus Backend — Codebase Context\n")
    lines.append(f"_Generated by `scripts/dev-context.py` — {len(data)} source files_\n")
    lines.append("---\n")

    for file in data:
        rel = file["file"]
        lines.append(f"## `{rel}`\n")

        if file["includes"]:
            deps = [i for i in file["includes"] if not i.startswith("<")]
            if deps:
                lines.append(f"- **local deps:** {', '.join(deps)}")
            sys_includes = [i for i in file["includes"] if i.startswith("<")]
            if sys_includes:
                lines.append(f"- **sys deps:** {', '.join(sys_includes)}")
            lines.append("")

        ns_map: dict[str, list] = {}

        # Group classes by namespace
        for cls in file["classes"]:
            ns = "::".join(cls["namespace"])
            ns_map.setdefault(ns, []).append(cls)

        for enum in file["enums"]:
            ns = "::".join(enum["namespace"])
            ns_map.setdefault(ns, []).append(enum)

        for ff in file["free_functions"]:
            ns = "::".join(ff["namespace"])
            ns_map.setdefault(ns, []).append(ff)

        for fd in file["free_declarations"]:
            ns = "::".join(fd["namespace"])
            ns_map.setdefault(ns, []).append(fd)

        # Sort: <global> first, then alphabetical
        ordered_ns = sorted(ns_map.keys(), key=lambda x: (x == "<global>", x))

        for ns in ordered_ns:
            items = ns_map[ns]
            if ns != "<global>":
                lines.append(f"### namespace `{ns}`\n")

            for item in items:
                if isinstance(item, dict) and "kind" in item and item["kind"] in ("class", "struct"):
                    cls = item
                    template_tag = "template " if cls.get("template") else ""
                    lines.append(f"**{template_tag}{cls['kind']} `{cls['name']}`**{cls['bases']}")
                    by_access = {"public": [], "private": [], "protected": []}
                    for m in cls["methods"]:
                        by_access.setdefault(m["access"], []).append(m["signature"])
                    for access in ("public", "private", "protected"):
                        if by_access[access]:
                            lines.append(f"  - {access}:")
                            for sig in by_access[access]:
                                lines.append(f"    - `{sig}`")
                    for m in cls["members"]:
                        lines.append(f"  - {m['access']} member: `{m['decl']}`")
                    lines.append("")

                elif isinstance(item, dict) and "decl" in item:
                    lines.append(f"- {item['decl']}")
                    lines.append("")

                elif isinstance(item, dict) and "signature" in item:
                    lines.append(f"- `{item['signature']}`")
                    lines.append("")

        # If nothing was in any namespace, still print something
        if not ns_map:
            lines.append("_(no symbols extracted)_\n")

    return "\n".join(lines)


# ── CLI ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Generate a compact Markdown context map of the C++ codebase."
    )
    parser.add_argument(
        "--src",
        default=str(SRC),
        help=f"Source directory (default: {SRC})",
    )
    parser.add_argument(
        "--output",
        "-o",
        help="Output file (default: stdout)",
    )
    args = parser.parse_args()

    src_dir = Path(args.src).resolve()
    files = sorted(src_dir.rglob("*.cc")) + sorted(src_dir.rglob("*.hxx"))

    results = []
    for fpath in files:
        try:
            result = extract_file(fpath)
            results.append(result)
        except Exception as e:
            print(f"⚠  Error parsing {fpath}: {e}", file=sys.stderr)

    md = format_markdown(results)

    if args.output:
        Path(args.output).write_text(md)
        print(f"✓  Written to {args.output}", file=sys.stderr)
    else:
        sys.stdout.write(md)


if __name__ == "__main__":
    main()
