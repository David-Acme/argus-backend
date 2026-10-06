#!/usr/bin/env python3
import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[4]
ROLE_HEADERS = ("packages/lib/auth/src/auth/role-access.hxx", "packages/lib/auth/src/auth/capability.hxx")
MODULE_HEADERS = ("packages/lib/auth/src/auth/module-snapshot.hxx", "packages/lib/auth/src/auth/role-access.hxx")
CAPABILITY_HEADER = "packages/lib/auth/src/auth/capability.hxx"
TOOL_SOURCES = (
    "services/productivity/src/feature/mcp/services/productivity-tools.cc",
    "services/settings/src/feature/mcp/services/module-tools.cc",
    "services/llm/src/feature/memory/services/memory/memory-tool-descriptors.cc",
    "services/llm/src/feature/llm/services/tools/core-tools.cc",
    "services/camera/src/feature/mcp/services/camera-tools.cc",
    "services/guard/src/feature/mcp/services/guard-tools.cc",
)
ROLES = ("Owner", "Resident", "Guard", "Guest")
BASELINE = "baseline"


def read(root, relative):
    return (root / relative).read_text()


def evaluate(expression, table):
    expression = " ".join(expression.split())
    wrapped = re.fullmatch(r"static_cast<RoleMask>\((.*)\)", expression)
    if wrapped:
        return evaluate(wrapped.group(1), table)
    many = re.fullmatch(r"roleBits\(\{(.*)\}\)", expression)
    if many:
        return set(re.findall(r"UserRole::(\w+)", many.group(1)))
    single = re.fullmatch(r"roleBit\(UserRole::(\w+)\)", expression)
    if single:
        return {single.group(1)}
    if "|" in expression:
        out = set()
        for part in expression.split("|"):
            out |= evaluate(part, table)
        return out
    if expression == "kBaselineBit":
        return {BASELINE}
    if expression in table:
        return set(table[expression])
    raise ValueError(f"cannot evaluate the role expression: {expression}")


def role_constants(root):
    table = {}
    for header in ROLE_HEADERS:
        for name, expression in re.findall(r"inline constexpr RoleMask (\w+) =\s*([^;]+);", read(root, header)):
            if name != "kBaselineBit":
                table[name] = evaluate(expression, table)
    return table


def module_constants(root):
    out = {}
    for header in MODULE_HEADERS:
        out.update(re.findall(r"inline constexpr std::string_view (k\w+Module) = \"(\w+)\";", read(root, header)))
    return out


def capabilities(root):
    roles, modules = role_constants(root), module_constants(root)
    found = {}
    for identifier, module, mask in re.findall(
            r"\{\.id = \"([\w.]+)\", \.module = (\w+), \.roles = (\w+)\}", read(root, CAPABILITY_HEADER)):
        if identifier in found:
            raise ValueError(f"capability {identifier} is declared twice")
        found[identifier] = {"module": modules[module], "roles": sorted(roles[mask] - {BASELINE})}
    return found


def tool_specs(root):
    specs = {}
    for relative in TOOL_SOURCES:
        text = read(root, relative)
        local = dict(re.findall(r"constexpr (?:const char\*|std::string_view) (k\w+) = \"(\w*)\";", text))
        marks = list(re.finditer(r"\.name = \"([a-z_]+\.[a-z_]+)\"", text))
        for index, mark in enumerate(marks):
            end = marks[index + 1].start() if index + 1 < len(marks) else len(text)
            body = text[mark.start():end]
            capability = re.search(r"\.capability = \"([\w.]+)\"", body)
            if not capability:
                continue
            module = re.search(r"\.module = (?:\"(\w*)\"|(k\w+))", body)
            annotations = re.search(r"\.annotations = \{([^}]*)\}", body)
            spec = {"capability": capability.group(1),
                    "declaredModule": (module.group(1) if module.group(1) is not None else local[module.group(2)]) if module else "",
                    "readOnly": bool(annotations and ".readOnly = true" in annotations.group(1)),
                    "source": relative}
            name = mark.group(1)
            if name in specs and specs[name] != spec:
                raise ValueError(f"tool {name} is declared twice with different specs")
            specs[name] = spec
    return specs


def parse(root=ROOT):
    root = pathlib.Path(root)
    known, specs = capabilities(root), tool_specs(root)
    tools, problems = {}, []
    for name, spec in sorted(specs.items()):
        capability = known.get(spec["capability"])
        if capability is None:
            problems.append(f"{name} requires {spec['capability']}, which kCapabilities does not declare")
            continue
        if spec["declaredModule"] not in ("", capability["module"]):
            problems.append(f"{name} declares module {spec['declaredModule']} but {spec['capability']} belongs to "
                            f"{capability['module']}")
        tools[name] = {"capability": spec["capability"], "module": capability["module"]}
    roles = {role.lower(): sorted(name for name, tool in tools.items()
                                  if role in known[tool["capability"]]["roles"]) for role in ROLES}
    return {"tools": tools, "roles": roles}, problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--write")
    args = parser.parse_args()
    document, problems = parse()
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    text = json.dumps(document, indent=2, sort_keys=True) + "\n"
    if args.write:
        pathlib.Path(args.write).write_text(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
