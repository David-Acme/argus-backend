#!/usr/bin/env python3
"""Section 2.4's tier table, checked mechanically (plan section 4.13, rule 7).

Every ``CMakeLists.txt`` that belongs to the tree under ``packages/`` and
``services/`` is read and the first-party edges are collected from **both**
places one can be written: the ``DEPENDS`` / ``SYSTEM_DEPENDS`` / ``MODULES``
keyword lists of the ``argus_*`` helpers -- where a first-party dependency
actually lives in this tree -- and literal ``target_link_libraries()`` calls,
which is what rule 7 names. A scan that reads only ``target_link_libraries``
sees almost none of the graph (measured in phase 2 step 4).

The exit status is the gate: non-zero when an edge the table can classify is
forbidden, when the unit graph has a cycle, or when an item names the
first-party ``argus::`` namespace and no declaration answers it. Edges that
touch a package section 9.1 has not moved yet (``audit``, ``identity``,
``intent``, ``memory``, ``room``, ``socket``, ``sync``) are outside the table's
reach today: they are counted, grouped by the step that owns each move, and
never fail the build -- the phase that moves them is the one that makes them
legal or illegal. A run that read nothing exits 2 rather than reporting the
zeroes that look like a clean tree.
"""

import argparse
import os
import re
import subprocess
import sys
from collections import defaultdict

# Section 2.4's table. Tier 1 is lib/ (thirteen packages), tier 2 is
# contracts/* plus lib/http, tier 3 is clients/*, tier 4 is lib/auth alone,
# tier 5 is services/*. Anything else under packages/ is a package section 9.1
# has not moved yet and has no tier until it does.
IN_TRANSIT = ("packages/identity", "packages/intent",
              "packages/memory", "packages/sync")

# What each tier may depend on -- rule 1 read together with the table's "may
# never" column: a tier never points back up, tier 3 never reaches another
# client, tier 5 never reaches another service.
ALLOWED = {1: {1}, 2: {1, 2}, 3: {1, 2}, 4: {1, 2, 3}, 5: {1, 2, 3, 4}}

HELPERS = {
    "argus_lib": "lib",
    "argus_contracts": "contracts",
    "argus_clients": "clients",
    "argus_client_module": "clients",
    "argus_module": "",
    "argus_service": None,  # an executable named by NAME, no argus:: alias
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
    """The unit rule 1 is about: the package for tiers 1-4, the service for 5."""
    if pkg.startswith("services/"):
        return "/".join(pkg.split("/")[:2])
    return pkg


def is_in_transit(pkg):
    """A package section 9.1 has not moved yet -- including its nested tools."""
    return any(pkg == name or pkg.startswith(name + "/") for name in IN_TRANSIT)


def same_unit(one, other):
    return unit_of(one) == unit_of(other)


def balanced(src, start):
    """src[start] is '(' -> return the body up to its matching ')'."""
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
    """Every CMakeLists.txt that belongs to the tree under packages/ and
    services/: the tracked ones, and any new ones not committed yet.

    Tracked files are the repository's own; the ignored ones are build trees.
    A package that has just been written but not staged belongs to the tree as
    well, and the phases that add packages would otherwise be checked only
    after a commit -- so untracked-but-not-ignored files are read too, and the
    two directories that hold foreign sources are filtered out whatever the
    ignore rules say. Outside a checkout (a Docker build context, a test
    fixture) the tree is walked instead.
    """
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
    """The list items in a helper body.

    CMake separates list items with whitespace or a semicolon and both are
    legal here, so ``DEPENDS a;b`` names two dependencies: a tokeniser that
    split on whitespace alone would hand the resolver ``a;b`` as one name, and
    a first-party edge written that way would be counted as a mention of some
    foreign library instead of judged. A generator expression is one item
    however many semicolons it holds, so one is never split.
    """
    items = []
    for token in body.split():
        if token.startswith("$<") and token.endswith(">"):
            items.append(token)
        else:
            items.extend(piece for piece in token.split(";") if piece)
    return items


def collect(root, files):
    """Returns the declarations, the alias table and the raw target table."""
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
                    # argus_service creates the executable the NAME gives, inside
                    # the helper, so the package's own file never says
                    # add_executable: without this line the one spelling a
                    # tier-3 -> tier-5 edge is written in (a bare service name)
                    # would be counted as a third-party mention instead.
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
    """One link item -> one edge, or one third-party mention.

    A link item can be wrapped: ``$<LINK_LIBRARY:feature,libs>`` asks for the
    libraries with a linking feature, and the libraries inside it are edges like
    any other (services/gateway links its own core that way).

    An item that spells the first-party namespace and resolves to nothing is an
    error rather than a mention of something foreign: ``argus::`` is this
    tree's own namespace, so a name it does not declare is a typo or a name
    that changed, and counting it quietly would hide exactly the edge this
    check exists to judge.
    """
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
    """Every cycle in a directed graph, as unit paths."""
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
    # A run that examined nothing is not a run that passed: a floor of zero
    # makes every count below it zero too, which reads exactly like a green
    # tree.
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

    # Rule 1 forbids a cycle whatever the tier of its ends, so the graph is
    # built from every edge rather than from the classified ones: the tier
    # decides whether an edge is *allowed*, never whether it may exist, and a
    # cycle through a package section 9.1 has not moved yet violates the same
    # rule. Measured on this tree, which is why the wider graph costs nothing:
    # 39 units and 217 edges instead of 32 and 141, and 0 cycles either way.
    graph = defaultdict(set)
    for package, item, dependency, dependency_tier in edges:
        one, other = unit_of(package), unit_of(dependency)
        if one != other:
            graph[one].add(other)
    cycles = find_cycles(graph)

    unclassified = sorted({os.path.dirname(name) for name in files
                           if name.startswith("packages/") and
                           tier_of_package(os.path.dirname(name)) is None and
                           not is_in_transit(os.path.dirname(name))})

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
        print("check-deps: packages with no tier yet: "
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
