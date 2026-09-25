#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
import sys
from collections import defaultdict


ALLOWED = {1: {1}, 2: {1, 2}, 3: {1, 2}, 4: {1, 2, 3}, 5: {1, 2, 3, 4}}

HELPERS = {
    "argus_lib": "lib",
    "argus_contracts": "contracts",
    "argus_clients": "clients",
    "argus_client_module": "clients",
    "argus_module": "",
    "argus_service": None,
}

KEYWORDS = {"NAME", "GROUP", "MAIN", "PROTO_ROOT", "PROTO", "SOURCES",
            "INCLUDES", "DEPENDS", "SYSTEM_DEPENDS", "MODULES", "PORTS"}
DEP_KEYS = ("DEPENDS", "SYSTEM_DEPENDS", "MODULES")


def tier_of_package(pkg):
    if pkg == "packages/lib/http":
        return 2
    if pkg == "packages/lib/auth":
        return 4
    if pkg.startswith("packages/lib/"):
        return 1
    if pkg.startswith("packages/contracts"):
        return 2
    if pkg.startswith("packages/clients"):
        return 3
    if pkg.startswith("services/"):
        return 5
    return None


def unit_of(pkg):
    if pkg.startswith("services/"):
        return "/".join(pkg.split("/")[:2])
    return pkg


def same_unit(one, other):
    return unit_of(one) == unit_of(other)


def balanced(src, start):
    index, depth = start, 0
    while index < len(src):
        if src[index] == "(":
            depth += 1
        elif src[index] == ")":
            depth -= 1
            if depth == 0:
                return src[start + 1:index]
        index += 1
    raise ValueError("unbalanced parentheses")


def cmake_lists(root):
    try:
        listed = subprocess.run(["git", "ls-files", "--cached", "--others",
                                 "--exclude-standard", "--", "packages",
                                 "services"], cwd=root, text=True,
                                capture_output=True, check=True).stdout.split()
        files = [f for f in listed if f.endswith("CMakeLists.txt") and
                 not any(part in ("build", "third_party")
                         for part in f.split("/"))]
        if files:
            return sorted(files)
    except (OSError, subprocess.CalledProcessError):
        pass
    files = []
    for top in ("packages", "services"):
        for base, dirs, names in os.walk(os.path.join(root, top)):
            dirs[:] = [d for d in dirs
                       if d not in ("build", "third_party", ".git")]
            if "CMakeLists.txt" in names:
                files.append(os.path.relpath(os.path.join(base,
                                                          "CMakeLists.txt"),
                                             root))
    return sorted(files)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def tokens_of(body):
    items = []
    for token in body.split():
        if token.startswith("$<") and token.endswith(">"):
            items.append(token)
        else:
            items.extend(piece for piece in token.split(";") if piece)
    return items


def collect(root, files):
    alias_pkg, alias_tier, target_pkg = {}, {}, defaultdict(set)
    declarations = []
    for name in files:
        package = os.path.dirname(name)
        src = read(os.path.join(root, name))
        for helper, default_group in HELPERS.items():
            for match in re.finditer(re.escape(helper) + r"\s*\(", src):
                body = balanced(src, match.end() - 1)
                current, args = None, defaultdict(list)
                for token in tokens_of(body):
                    if token in KEYWORDS:
                        current = token
                    elif current:
                        args[current].append(token)
                if not args["NAME"]:
                    continue
                line = src[:match.start()].count("\n") + 1
                if default_group is None:
                    alias, raw = args["NAME"][0], args["NAME"][0]
                    target_pkg[raw].add(package)
                else:
                    group = args["GROUP"][0] if args["GROUP"] else default_group
                    name = args["NAME"][0]
                    prefix = f"{group}_" if group else ""
                    alias = f"argus::{group}::{name}" if group else f"argus::{name}"
                    raw = f"argus_{prefix}{name}"
                    alias_pkg[alias] = package
                    alias_tier[alias] = tier_of_package(package)
                    target_pkg[raw].add(package)
                declarations.append((package, alias, line, args))
        for match in re.finditer(r"add_library\(([A-Za-z0-9_.:-]+)", src):
            target_pkg[match.group(1)].add(package)
        for match in re.finditer(r"add_executable\(([A-Za-z0-9_.:-]+)", src):
            target_pkg[match.group(1)].add(package)
    return declarations, alias_pkg, alias_tier, target_pkg


def resolve(item, package, alias_pkg, alias_tier, target_pkg, edges, foreign,
            unresolved, where):
    wrapped = re.match(r"^\$<LINK_LIBRARY:[^,]+,(.*)>$", item)
    if wrapped:
        for piece in re.split(r"[,;]", wrapped.group(1)):
            if piece:
                resolve(piece, package, alias_pkg, alias_tier, target_pkg, edges,
                        foreign, unresolved, where)
        return
    if item.startswith("argus::"):
        dependency = alias_pkg.get(item)
        if dependency is None:
            unresolved[(package, item, where)] += 1
        elif dependency != package:
            edges.append((package, item, dependency, alias_tier[item]))
    elif item in target_pkg:
        for dependency in target_pkg[item]:
            if dependency != package:
                edges.append((package, item, dependency, tier_of_package(dependency)))
    else:
        foreign[item.split("::")[0]] += 1


def scan(root):
    files = cmake_lists(root)
    declarations, alias_pkg, alias_tier, target_pkg = collect(root, files)
    edges, foreign = [], defaultdict(int)
    unresolved = defaultdict(int)
    site = {}
    for package, alias, line, args in declarations:
        where = f"{package}/CMakeLists.txt:{line}"
        for key in DEP_KEYS:
            for item in args[key]:
                before = len(edges)
                resolve(item, package, alias_pkg, alias_tier, target_pkg, edges,
                        foreign, unresolved, where)
                for edge in edges[before:]:
                    site[(edge[0], edge[1], edge[2])] = (package, alias, key, line)
    for name in files:
        package = os.path.dirname(name)
        src = read(os.path.join(root, name))
        for match in re.finditer(r"target_link_libraries\s*\(", src):
            body = balanced(src, match.end() - 1)
            tokens = tokens_of(body)
            if not tokens:
                continue
            target, rest = tokens[0], tokens[1:]
            if rest and rest[0] in ("PUBLIC", "PRIVATE", "INTERFACE"):
                rest = rest[1:]
            line = src[:match.start()].count("\n") + 1
            where = f"{name}:{line}"
            for item in rest:
                before = len(edges)
                resolve(item, package, alias_pkg, alias_tier, target_pkg, edges,
                        foreign, unresolved, where)
                for edge in edges[before:]:
                    site[(edge[0], edge[1], edge[2])] = (package, target, "tll",
                                                         line)
    return files, declarations, edges, foreign, unresolved, site


def find_cycles(graph):
    WHITE, GREY, BLACK = 0, 1, 2
    colour, stack, cycles = defaultdict(int), [], []

    def visit(unit):
        colour[unit] = GREY
        stack.append(unit)
        for peer in sorted(graph[unit]):
            if colour[peer] == GREY:
                cycles.append(tuple(stack[stack.index(peer):] + [peer]))
            elif colour[peer] == WHITE:
                visit(peer)
        stack.pop()
        colour[unit] = BLACK

    for unit in sorted(graph):
        if colour[unit] == WHITE:
            visit(unit)
    return cycles


def main():
    parser = argparse.ArgumentParser(description="Section 2.4's tier table.")
    parser.add_argument("--root", default=os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))),
        help="the tree to read (default: the repository root)")
    parser.add_argument("--list-deferred", action="store_true",
                        help="print every edge deferred to section 9.1")
    args = parser.parse_args()
    root = os.path.abspath(args.root)
    if not os.path.isdir(os.path.join(root, "packages")):
        print(f"check-deps: no packages/ under {root}", file=sys.stderr)
        return 2

    files, declarations, edges, foreign, unresolved, site = scan(root)
    if not files:
        print(f"check-deps: no CMakeLists.txt under {root} -- nothing was "
              f"checked", file=sys.stderr)
        return 2
    if not declarations:
        print(f"check-deps: no argus_* declaration in the {len(files)} "
              f"CMakeLists.txt found -- nothing was checked", file=sys.stderr)
        return 2
    forbidden, deferred = [], []
    for package, item, dependency, dependency_tier in edges:
        linker_tier = tier_of_package(package)
        if linker_tier is None or dependency_tier is None:
            deferred.append((package, item, dependency, linker_tier,
                             dependency_tier))
            continue
        if not same_unit(package, dependency) and \
                dependency_tier not in ALLOWED[linker_tier]:
            forbidden.append((package, item, dependency, linker_tier,
                              dependency_tier))

    graph = defaultdict(set)
    for package, item, dependency, dependency_tier in edges:
        one, other = unit_of(package), unit_of(dependency)
        if one != other:
            graph[one].add(other)
    cycles = find_cycles(graph)

    unclassified = sorted({os.path.dirname(name) for name in files
                           if name.startswith("packages/") and
                           tier_of_package(os.path.dirname(name)) is None})

    for package, item, dependency, linker_tier, dependency_tier in sorted(
            forbidden, key=lambda e: (e[3], e[0], e[1])):
        package_dir, target, key, line = site[(package, item, dependency)]
        where = f"{package_dir}/CMakeLists.txt:{line}"
        print(f"forbidden: T{linker_tier} -> T{dependency_tier}  "
              f"{package}  ({key} of {target}) -> {item}  "
              f"[declared in {dependency}]  {where}", file=sys.stderr)
    for cycle in cycles:
        print("forbidden: cycle  " + " -> ".join(cycle), file=sys.stderr)
    for package, item, where in sorted(unresolved):
        print(f"unresolved: {package} names {item}, which this tree does not "
              f"declare  {where}", file=sys.stderr)

    print(f"check-deps: {len(declarations)} declarations, {len(edges)} edges, "
          f"{len(forbidden)} forbidden, {len(cycles)} cycles, "
          f"{len(unresolved)} unresolved, "
          f"{len(deferred)} edges deferred to phase 3 "
          f"({sum(foreign.values())} third-party mentions over "
          f"{len(foreign)} roots)")
    if unclassified:
        print("check-deps: packages outside the three groups: "
              + ", ".join(unclassified))
    if args.list_deferred:
        grouped = defaultdict(list)
        for package, item, dependency, linker_tier, dependency_tier in deferred:
            grouped[(package, item, dependency)].append(1)
        for (package, item, dependency) in sorted(grouped):
            package_dir, target, key, line = site[(package, item, dependency)]
            print(f"deferred: {package} -> {item} [{dependency}]  "
                  f"{package_dir}/CMakeLists.txt:{line}")
    return 1 if forbidden or cycles or unresolved else 0


if __name__ == "__main__":
    sys.exit(main())
